// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

namespace File
{
enum class AccessMode
{
  Read,
  Write,
  ReadAndWrite,
};

enum class OpenMode
{
  // Write: Truncate; Read and ReadAndWrite: Existing.
  Default,
  // Open the file, creating it if needed.
  Always,
  // Like Always, and empty an existing file.
  Truncate,
  // Fail unless the file exists.
  Existing,
  // Fail if the file exists; create it.
  Create,
};

// A file handle (Win32 HANDLE / POSIX descriptor) that keeps its own position and does every
// transfer at an explicit offset (overlapped I/O / pread / pwrite), never through the OS file
// position. Copies duplicate the OS handle, so separate copies can be used from different
// threads.
class DirectIOFile final
{
public:
  DirectIOFile();
  ~DirectIOFile();

  DirectIOFile(const DirectIOFile&);
  DirectIOFile& operator=(const DirectIOFile&);
  DirectIOFile(DirectIOFile&&);
  DirectIOFile& operator=(DirectIOFile&&);

  explicit DirectIOFile(const std::string& path, AccessMode access_mode,
                        OpenMode open_mode = OpenMode::Default);

  // `path` is UTF-8. The object must not already be open. Read + Truncate is refused.
  bool Open(const std::string& path, AccessMode access_mode,
            OpenMode open_mode = OpenMode::Default);

  // false if it was not open.
  bool Close();

  bool IsOpen() const;

  // Transfers exactly `size` bytes at `offset`, ignoring and not changing the current position.
  // Safe to call concurrently on the same object.
  bool OffsetRead(u64 offset, u8* out_ptr, u64 size);
  bool OffsetRead(u64 offset, std::span<u8> out_data)
  {
    return OffsetRead(offset, out_data.data(), out_data.size());
  }
  bool OffsetWrite(u64 offset, const u8* in_ptr, u64 size);
  bool OffsetWrite(auto offset, std::span<const u8> in_data)
  {
    return OffsetWrite(offset, in_data.data(), in_data.size());
  }

  // Transfers at the current position and advances it on success.
  bool Read(u8* out_ptr, u64 size)
  {
    if (!OffsetRead(m_current_offset, out_ptr, size))
      return false;
    m_current_offset += size;
    return true;
  }
  bool Read(std::span<u8> out_data) { return Read(out_data.data(), out_data.size()); }

  bool Write(const u8* in_ptr, u64 size)
  {
    if (!OffsetWrite(m_current_offset, in_ptr, size))
      return false;
    m_current_offset += size;
    return true;
  }
  bool Write(std::span<const u8> in_data) { return Write(in_data.data(), in_data.size()); }

  // 0 on error.
  u64 GetSize() const;

  // Fails (leaving the position alone) if the result would be negative or the file is closed.
  // Seeking past the end is allowed.
  bool Seek(s64 offset, SeekOrigin origin);

  // 0 when not open.
  u64 Tell() const { return m_current_offset; }

  // Flushes written data to the disk.
  bool Flush();

  auto GetHandle() const
  {
#if defined(_WIN32)
    return m_handle;
#else
    return m_fd;
#endif
  }

private:
  void Swap(DirectIOFile& other);
  DirectIOFile Duplicate() const;

#if defined(_WIN32)
  // HANDLE without including windows.h; -1 is INVALID_HANDLE_VALUE.
  using HandleType = void*;
  HandleType m_handle{HandleType(-1)};
#else
  using HandleType = int;
  HandleType m_fd{-1};
#endif

  u64 m_current_offset{};
};

// Operations through an open handle, so they work while we hold the file open (on Windows a path
// based rename/delete of an open file would otherwise fail).

bool Resize(DirectIOFile& file, u64 size);

// Replaces an existing destination. Windows renames through `file`; elsewhere `source_path` is
// renamed and `file` only has to be open.
bool Rename(DirectIOFile& file, const std::string& source_path,
            const std::string& destination_path);
// Windows deletes through `file`; elsewhere `filename` is deleted and `file` only has to be open.
bool Delete(DirectIOFile& file, const std::string& filename);
}  // namespace File
