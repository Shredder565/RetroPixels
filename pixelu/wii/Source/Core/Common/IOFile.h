// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

#include "Common/CommonTypes.h"

namespace File
{
enum class SeekOrigin
{
  Begin,
  Current,
  End,
};

// How other processes may access a file while we have it open (Windows only).
enum class SharedAccess
{
  Default,  // the CRT default for fopen_s: no sharing
  Read,     // others may read, but not write
};

// Owns a C stdio FILE* and tracks whether any operation on it has failed.
//
// Every operation that fails (including any operation on a closed file) clears the "good" flag;
// it stays cleared until ClearError() or a successful Open()/SetHandle().
class IOFile
{
public:
  IOFile();
  IOFile(std::FILE* file);
  IOFile(const std::string& filename, const char openmode[],
         SharedAccess sh = SharedAccess::Default);

  ~IOFile();

  IOFile(const IOFile&) = delete;
  IOFile& operator=(const IOFile&) = delete;

  IOFile(IOFile&& other) noexcept;
  IOFile& operator=(IOFile&& other) noexcept;

  void Swap(IOFile& other) noexcept;

  // `filename` is UTF-8; `openmode` as for fopen.
  bool Open(const std::string& filename, const char openmode[],
            SharedAccess sh = SharedAccess::Default);
  bool Close();

  // Reads exactly `count` elements; a short read counts as failure. *num_read (if given)
  // receives the number actually read.
  template <typename T>
  requires(std::is_trivially_copyable_v<T>)
  bool ReadArray(T* elements, size_t count, size_t* num_read = nullptr)
  {
    const size_t done = IsOpen() ? std::fread(elements, sizeof(T), count, m_file) : 0;
    if (!IsOpen() || done != count)
      m_good = false;
    if (num_read)
      *num_read = done;
    return m_good;
  }

  template <typename T>
  requires(std::is_trivially_copyable_v<T>)
  bool WriteArray(const T* elements, size_t count)
  {
    if (!IsOpen() || std::fwrite(elements, sizeof(T), count, m_file) != count)
      m_good = false;
    return m_good;
  }

  template <typename T, std::size_t N>
  bool ReadArray(std::array<T, N>* elements, size_t* num_read = nullptr)
  {
    return ReadArray(elements->data(), N, num_read);
  }

  template <typename T, std::size_t N>
  bool WriteArray(const std::array<T, N>& elements)
  {
    return WriteArray(elements.data(), N);
  }

  bool ReadBytes(void* data, size_t length) { return ReadArray(static_cast<char*>(data), length); }
  bool WriteBytes(const void* data, size_t length)
  {
    return WriteArray(static_cast<const char*>(data), length);
  }
  bool WriteString(std::string_view str) { return WriteBytes(str.data(), str.size()); }

  bool IsOpen() const { return m_file != nullptr; }
  bool IsGood() const { return m_good; }
  explicit operator bool() const { return IsGood() && IsOpen(); }

  std::FILE* GetHandle() { return m_file; }
  // Closes the current file (if any) and takes ownership of `file`.
  void SetHandle(std::FILE* file);

  bool Seek(s64 offset, SeekOrigin origin);
  // Current position, or UINT64_MAX when closed.
  u64 Tell() const;
  // File size, or 0 when closed.
  u64 GetSize() const;
  bool Resize(u64 size);
  bool Flush();

  void ClearError()
  {
    m_good = true;
    if (IsOpen())
      std::clearerr(m_file);
  }

private:
  std::FILE* m_file;
  bool m_good;
};
}  // namespace File
