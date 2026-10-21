// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The Wii's system configuration file, /shared2/sys/SYSCONF on the NAND.
//
// Layout (WiiBrew "/shared2/sys/SYSCONF"), all big-endian, exactly 0x4000 bytes:
//   "SCv0", u16 entry count, u16 offset of each entry plus one past the last,
//   the entries, zero padding, "SCed" in the last four bytes.
// Each entry starts with one byte (type << 5 | (name length - 1)) and the name;
// array types then store their data length minus one (u16 for big arrays,
// u8 for small ones), and every type is followed by its data.

#pragma once

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/NandPaths.h"
#include "Common/Swap.h"

namespace IOS::HLE::FS
{
class FileHandle;
class FileSystem;
}  // namespace IOS::HLE::FS

class SysConf final
{
public:
  // Reads the file straight away (or starts from the defaults); the destructor writes it back.
  explicit SysConf(std::shared_ptr<IOS::HLE::FS::FileSystem> fs);
  ~SysConf();

  void Clear();
  void Load();
  bool Save() const;

  struct Entry
  {
    // Stored in the top three bits of each entry's first byte.
    enum Type : u8
    {
      BigArray = 1,
      SmallArray = 2,
      Byte = 3,
      Short = 4,
      Long = 5,
      LongLong = 6,
      // A boolean; not called Bool because of a clashing macro.
      ByteBool = 7,
    };

    Entry(Type type_, std::string name_);
    Entry(Type type_, std::string name_, std::vector<u8> bytes_);

    // Scalar entries: the big-endian value, or default_value when the entry
    // does not hold exactly sizeof(T) bytes.
    template <typename T>
    T GetData(T default_value) const
    {
      if (bytes.size() != sizeof(T))
        return default_value;
      T raw;
      std::memcpy(&raw, bytes.data(), sizeof(T));
      return Common::FromBigEndian(raw);
    }

    template <typename T>
    void SetData(T value)
    {
      ASSERT(sizeof(value) == bytes.size());
      const T raw = Common::FromBigEndian(value);
      std::memcpy(bytes.data(), &raw, bytes.size());
    }

    Type type;
    std::string name;
    std::vector<u8> bytes;
  };

  Entry& AddEntry(Entry&& entry);
  Entry* GetEntry(std::string_view key);
  const Entry* GetEntry(std::string_view key) const;
  Entry* GetOrAddEntry(std::string_view key, Entry::Type type);
  void RemoveEntry(std::string_view key);

  // Scalar shortcuts by entry name.
  template <typename T>
  T GetData(std::string_view key, T default_value) const
  {
    if (const Entry* entry = GetEntry(key))
      return entry->GetData(default_value);
    return default_value;
  }

  template <typename T>
  void SetData(std::string_view key, Entry::Type type, T value)
  {
    Entry* entry = GetOrAddEntry(key, type);
    entry->SetData(value);
  }

private:
  void InsertDefaultEntries();
  bool LoadFromFile(const IOS::HLE::FS::FileHandle& file);

  std::vector<Entry> m_entries;
  std::shared_ptr<IOS::HLE::FS::FileSystem> m_fs;
};
