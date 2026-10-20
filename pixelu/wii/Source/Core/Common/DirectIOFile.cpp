// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/DirectIOFile.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#ifdef _WIN32
#include <windows.h>

#include "Common/CommonFuncs.h"
#include "Common/StringUtil.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Common/FileUtil.h"
#endif

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

namespace File
{
DirectIOFile::DirectIOFile() = default;

DirectIOFile::~DirectIOFile()
{
  Close();
}

DirectIOFile::DirectIOFile(const DirectIOFile& other)
{
  *this = other.Duplicate();
}

DirectIOFile& DirectIOFile::operator=(const DirectIOFile& other)
{
  return *this = other.Duplicate();
}

DirectIOFile::DirectIOFile(DirectIOFile&& other)
{
  Swap(other);
}

DirectIOFile& DirectIOFile::operator=(DirectIOFile&& other)
{
  Close();
  Swap(other);
  return *this;
}

DirectIOFile::DirectIOFile(const std::string& path, AccessMode access_mode, OpenMode open_mode)
{
  Open(path, access_mode, open_mode);
}

bool DirectIOFile::Open(const std::string& path, AccessMode access_mode, OpenMode open_mode)
{
  ASSERT(!IsOpen());

  if (open_mode == OpenMode::Default)
    open_mode = access_mode == AccessMode::Write ? OpenMode::Truncate : OpenMode::Existing;

  // Truncating a file we may not write to means different things on different OSes; refuse.
  if (access_mode == AccessMode::Read && open_mode == OpenMode::Truncate)
    return false;

#ifdef _WIN32
  DWORD access = 0;
  if (access_mode != AccessMode::Write)
    access |= GENERIC_READ;
  if (access_mode != AccessMode::Read)
    access |= GENERIC_WRITE | DELETE;  // DELETE: so Rename()/Delete() work through the handle

  DWORD disposition = OPEN_ALWAYS;
  switch (open_mode)
  {
  case OpenMode::Truncate:
    disposition = CREATE_ALWAYS;
    break;
  case OpenMode::Create:
    disposition = CREATE_NEW;
    break;
  case OpenMode::Existing:
    disposition = OPEN_EXISTING;
    break;
  default:
    break;
  }

  // Full sharing, like a POSIX open.
  m_handle = CreateFileW(UTF8ToWString(path).c_str(), access,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!IsOpen())
    WARN_LOG_FMT(COMMON, "CreateFile: {}", Common::GetLastErrorString());
#else
  int flags = access_mode == AccessMode::Read  ? O_RDONLY :
              access_mode == AccessMode::Write ? O_WRONLY :
                                                 O_RDWR;
  switch (open_mode)
  {
  case OpenMode::Truncate:
    flags |= O_CREAT | O_TRUNC;
    break;
  case OpenMode::Create:
    flags |= O_CREAT | O_EXCL;
    break;
  case OpenMode::Existing:
    break;
  default:
    flags |= O_CREAT;
    break;
  }
  m_fd = open(path.c_str(), flags, 0666);
#endif

  return IsOpen();
}

bool DirectIOFile::Close()
{
  if (!IsOpen())
    return false;

  m_current_offset = 0;
#ifdef _WIN32
  return CloseHandle(std::exchange(m_handle, INVALID_HANDLE_VALUE)) != 0;
#else
  return close(std::exchange(m_fd, -1)) == 0;
#endif
}

bool DirectIOFile::IsOpen() const
{
#ifdef _WIN32
  return m_handle != INVALID_HANDLE_VALUE;
#else
  return m_fd != -1;
#endif
}

#ifdef _WIN32
namespace
{
// ReadFile/WriteFile at an explicit offset (an OVERLAPPED on a synchronous handle). They take a
// 32-bit length and may transfer less, so loop until done.
template <typename Ptr, typename Transfer>
bool TransferAt(HANDLE handle, u64 offset, Ptr data, u64 size, Transfer transfer)
{
  while (size != 0)
  {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);

    const DWORD request =
        static_cast<DWORD>(std::min<u64>(size, std::numeric_limits<DWORD>::max()));
    DWORD done = 0;
    if (!transfer(handle, data, request, &done, &position))
    {
      ERROR_LOG_FMT(COMMON, "OverlappedTransfer: {}", Common::GetLastErrorString());
      return false;
    }
    // A zero-byte read means end of file: not all requested data exists.
    if (done == 0)
      return false;

    size -= done;
    offset += done;
    data += done;
  }
  return true;
}
}  // namespace
#endif

bool DirectIOFile::OffsetRead(u64 offset, u8* out_ptr, u64 size)
{
#ifdef _WIN32
  return TransferAt(m_handle, offset, out_ptr, size,
                    [](HANDLE h, u8* p, DWORD n, DWORD* done, OVERLAPPED* o) {
                      return ReadFile(h, p, n, done, o) != 0;
                    });
#else
  return pread(m_fd, out_ptr, size, static_cast<off_t>(offset)) == static_cast<ssize_t>(size);
#endif
}

bool DirectIOFile::OffsetWrite(u64 offset, const u8* in_ptr, u64 size)
{
#ifdef _WIN32
  return TransferAt(m_handle, offset, in_ptr, size,
                    [](HANDLE h, const u8* p, DWORD n, DWORD* done, OVERLAPPED* o) {
                      return WriteFile(h, p, n, done, o) != 0;
                    });
#else
  return pwrite(m_fd, in_ptr, size, static_cast<off_t>(offset)) == static_cast<ssize_t>(size);
#endif
}

u64 DirectIOFile::GetSize() const
{
#ifdef _WIN32
  LARGE_INTEGER size{};
  if (GetFileSizeEx(m_handle, &size))
    return static_cast<u64>(size.QuadPart);
#else
  struct stat info{};
  if (fstat(m_fd, &info) == 0)
    return static_cast<u64>(info.st_size);
#endif
  return 0;
}

bool DirectIOFile::Seek(s64 offset, SeekOrigin origin)
{
  if (!IsOpen())
    return false;

  u64 base = 0;
  if (origin == SeekOrigin::Current)
    base = m_current_offset;
  else if (origin == SeekOrigin::End)
    base = GetSize();

  if (offset < 0 && static_cast<u64>(-offset) > base)
    return false;  // would go before the start

  m_current_offset = base + static_cast<u64>(offset);
  return true;
}

bool DirectIOFile::Flush()
{
#ifdef _WIN32
  return FlushFileBuffers(m_handle) != 0;
#else
  return fsync(m_fd) == 0;
#endif
}

void DirectIOFile::Swap(DirectIOFile& other)
{
#ifdef _WIN32
  std::swap(m_handle, other.m_handle);
#else
  std::swap(m_fd, other.m_fd);
#endif
  std::swap(m_current_offset, other.m_current_offset);
}

DirectIOFile DirectIOFile::Duplicate() const
{
  DirectIOFile copy;
  if (!IsOpen())
    return copy;

#ifdef _WIN32
  const HANDLE process = GetCurrentProcess();
  if (!DuplicateHandle(process, m_handle, process, &copy.m_handle, 0, FALSE,
                       DUPLICATE_SAME_ACCESS))
  {
    ERROR_LOG_FMT(COMMON, "DuplicateHandle: {}", Common::GetLastErrorString());
  }
#else
  copy.m_fd = dup(m_fd);
#endif
  ASSERT(copy.IsOpen());

  copy.m_current_offset = m_current_offset;
  return copy;
}

bool Resize(DirectIOFile& file, u64 size)
{
#ifdef _WIN32
  // Moves the OS file pointer, which nothing else in DirectIOFile uses; concurrent resizes need
  // outside synchronisation anyway.
  LARGE_INTEGER end;
  end.QuadPart = static_cast<LONGLONG>(size);
  return SetFilePointerEx(file.GetHandle(), end, nullptr, FILE_BEGIN) &&
         SetEndOfFile(file.GetHandle());
#else
  return ftruncate(file.GetHandle(), static_cast<off_t>(size)) == 0;
#endif
}

bool Rename(DirectIOFile& file, const std::string& source_path [[maybe_unused]],
            const std::string& destination_path)
{
#ifdef _WIN32
  // FILE_RENAME_INFO ends in a variable-length file name.
  const std::wstring target = UTF8ToWString(destination_path);
  const std::size_t name_bytes = target.size() * sizeof(wchar_t);
  const std::size_t total = offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t);

  // Allocated as u64s for the structure's alignment; zeroed, so the name ends in L'\0'.
  auto storage = std::make_unique<u64[]>((total + sizeof(u64) - 1) / sizeof(u64));
  auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.get());
  info->ReplaceIfExists = TRUE;
  info->RootDirectory = nullptr;
  info->FileNameLength = static_cast<DWORD>(name_bytes);  // bytes, without the terminator
  std::memcpy(info->FileName, target.data(), name_bytes);

  return SetFileInformationByHandle(file.GetHandle(), FileRenameInfo, info,
                                    static_cast<DWORD>(total)) != 0;
#else
  return file.IsOpen() && Rename(source_path, destination_path);
#endif
}

bool Delete(DirectIOFile& file, const std::string& filename [[maybe_unused]])
{
#ifdef _WIN32
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  return SetFileInformationByHandle(file.GetHandle(), FileDispositionInfo, &disposition,
                                    sizeof(disposition)) != 0;
#else
  return file.IsOpen() && Delete(filename, IfAbsentBehavior::NoConsoleWarning);
#endif
}
}  // namespace File
