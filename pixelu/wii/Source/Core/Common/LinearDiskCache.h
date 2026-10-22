// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Version.h"

// An append-only key/value file, used e.g. to cache compiled shaders between runs.
//
// File layout (native endianness):
//   header: char[4] "DCAC"; u16 sizeof(K); u16 sizeof(V); char[40] build revision (git hash,
//           not NUL-terminated). A file with any other header is discarded and recreated, so a
//           different build or key/value type starts a fresh cache.
//   entries, repeated:
//           u32 n; K key; V value[n]; u32 index (1 for the first entry, then 2, 3, ...)
// Reading stops at the first entry that is truncated or out of sequence; later appends then
// overwrite it.

namespace Common
{
template <typename K, typename V>
class LinearDiskCacheReader
{
public:
  virtual void Read(const K& key, const V* value, u32 value_size) = 0;
};

template <typename K, typename V>
class LinearDiskCache
{
public:
  // Opens (or creates) the cache file and passes every stored entry to `reader`. Returns the
  // number of entries read.
  u32 OpenAndRead(const std::string& filename, LinearDiskCacheReader<K, V>& reader)
  {
    static_assert(std::is_trivially_copyable_v<K>, "keys are read and written as raw bytes");

    Close();
    m_num_entries = 0;
    m_header.Init();

    m_file.Open(filename, "r+b");
    if (!m_file.IsOpen() || !HeaderMatches())
    {
      // Missing, unreadable or from another build: start over.
      Close();
      m_file.Open(filename, "wb");
      m_file.WriteArray(&m_header, 1);
      return 0;
    }

    const u64 file_size = m_file.GetSize();
    u64 end_of_valid_data = m_file.Tell();
    std::unique_ptr<V[]> value;

    for (;;)
    {
      u32 value_count = 0;
      if (!m_file.ReadArray(&value_count, 1))
        break;

      // Don't allocate for an entry that can't fit in the rest of the file.
      const u64 entry_rest = sizeof(K) + u64{value_count} * sizeof(V) + sizeof(u32);
      if (m_file.Tell() + entry_rest > file_size)
        break;

      K key;
      u32 index = 0;
      value = std::make_unique_for_overwrite<V[]>(value_count);
      if (!m_file.ReadArray(&key, 1) || !m_file.ReadArray(value.get(), value_count) ||
          !m_file.ReadArray(&index, 1) || index != m_num_entries + 1)
      {
        break;
      }

      end_of_valid_data = m_file.Tell();
      reader.Read(key, value.get(), value_count);
      ++m_num_entries;
    }

    // Continue appending right after the last good entry.
    m_file.ClearError();
    m_file.Seek(end_of_valid_data, File::SeekOrigin::Begin);
    return m_num_entries;
  }

  void Sync() { m_file.Flush(); }

  void Close()
  {
    if (m_file.IsOpen())
      m_file.Close();
  }

  // Appends an entry. Callers are expected not to append a key twice.
  void Append(const K& key, const V* value, u32 value_size)
  {
    m_file.WriteArray(&value_size, 1);
    m_file.WriteArray(&key, 1);
    m_file.WriteArray(value, value_size);
    ++m_num_entries;
    m_file.WriteArray(&m_num_entries, 1);
  }

private:
  bool HeaderMatches()
  {
    std::array<char, sizeof(Header)> on_disk;
    return m_file.ReadArray(on_disk.data(), on_disk.size()) &&
           std::memcmp(on_disk.data(), &m_header, sizeof(Header)) == 0;
  }

  struct Header
  {
    void Init()
    {
      std::memcpy(&id, "DCAC", sizeof(id));
      const std::string& revision = Common::GetScmRevGitStr();
      std::memcpy(ver, revision.data(), std::min(revision.size(), sizeof(ver)));
    }

    u32 id = 0;
    const u16 key_t_size = sizeof(K);
    const u16 value_t_size = sizeof(V);
    char ver[40] = {};
  } m_header;
  static_assert(sizeof(Header) == 48, "the header layout is part of the file format");

  File::IOFile m_file;
  u32 m_num_entries = 0;
};
}  // namespace Common
