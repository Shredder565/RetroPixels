// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/FS/HostBackend/FS.h"

#include <algorithm>
#include <cstdio>
#include <expected>
#include <memory>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

namespace IOS::HLE::FS
{
namespace
{
bool Allows(Mode granted, Mode wanted)
{
  return (static_cast<u8>(granted) & static_cast<u8>(wanted)) != 0;
}
}  // namespace

// Returns the one shared host stream for `host_path`, opening it if nobody has it open yet.
//
// IOS orders all accesses to a file strictly, so a write through one descriptor is visible at
// once through any other descriptor for the same file. Separate host streams would each buffer
// on their own and break that (some titles, including the System Menu, open the same file
// several times and depend on it). Sharing a single stream per path keeps the ordering.
// Only used from the CPU thread.
std::shared_ptr<File::IOFile> HostFileSystem::OpenHostFile(const std::string& host_path)
{
  if (const auto cached = m_open_files.find(host_path); cached != m_open_files.end())
    return cached->second.lock();

  // Opened read/write; each guest handle enforces its own mode in the calls below.
  File::IOFile stream;
  while (!stream.Open(host_path, "r+b"))
  {
    const bool retry = PanicYesNoFmt("Could not open \"{}\".\n"
                                     "Check the file's permissions and whether another program "
                                     "is using it.\n"
                                     "Choose \"Yes\" to try again.",
                                     host_path);
    if (!retry)
    {
      ERROR_LOG_FMT(IOS_FS, "Giving up opening host file {}", host_path);
      return nullptr;
    }
  }

  // When the last user lets go, close the stream and forget the cache entry.
  const auto release = [this, host_path](File::IOFile* raw) {
    delete raw;
    m_open_files.erase(host_path);
  };
  std::shared_ptr<File::IOFile> shared(new File::IOFile(std::move(stream)), release);
  m_open_files[host_path] = shared;
  return shared;
}

Result<FileHandle> HostFileSystem::OpenFile(Uid, Gid, const std::string& path, Mode mode)
{
  Handle* handle = AssignFreeHandle();
  if (handle == nullptr)
    return std::unexpected{ResultCode::NoFreeHandle};

  // Every failure below gives the slot back.
  const auto fail = [handle](ResultCode code) -> Result<FileHandle> {
    *handle = Handle{};
    return std::unexpected{code};
  };

  const std::string host_path = BuildFilename(path).host_path;
  if (File::IsDirectory(host_path))
    return fail(ResultCode::Invalid);
  if (!File::IsFile(host_path))
    return fail(ResultCode::NotFound);

  handle->host_file = OpenHostFile(host_path);
  if (!handle->host_file)
    return fail(ResultCode::AccessDenied);

  handle->wii_path = path;
  handle->mode = mode;
  handle->file_offset = 0;
  return FileHandle{this, ConvertHandleToFd(handle)};
}

ResultCode HostFileSystem::Close(Fd fd)
{
  Handle* handle = GetHandleFromFd(fd);
  if (handle == nullptr)
    return ResultCode::Invalid;

  // Dropping the shared stream closes it if this was its last user.
  *handle = Handle{};
  return ResultCode::Success;
}

Result<u32> HostFileSystem::ReadBytesFromFile(Fd fd, u8* ptr, u32 count)
{
  Handle* handle = GetHandleFromFd(fd);
  if (handle == nullptr || !handle->host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};
  if (!Allows(handle->mode, Mode::Read))
    return std::unexpected{ResultCode::AccessDenied};

  File::IOFile& stream = *handle->host_file;

  // Like IOS, clamp the request to the end of the file (in 32-bit arithmetic).
  const auto file_size = static_cast<u32>(stream.GetSize());
  if (handle->file_offset + count > file_size)
    count = file_size - handle->file_offset;

  // The stream may be shared, so always position it first.
  stream.Seek(handle->file_offset, File::SeekOrigin::Begin);
  std::FILE* const raw = stream.GetHandle();
  const auto got = static_cast<u32>(std::fread(ptr, 1, count, raw));
  if (got != count && std::ferror(raw))
    return std::unexpected{ResultCode::AccessDenied};

  // The position advances by what was actually read, not by what was asked for.
  handle->file_offset += got;
  return got;
}

Result<u32> HostFileSystem::WriteBytesToFile(Fd fd, const u8* ptr, u32 count)
{
  Handle* handle = GetHandleFromFd(fd);
  if (handle == nullptr || !handle->host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};
  if (!Allows(handle->mode, Mode::Write))
    return std::unexpected{ResultCode::AccessDenied};

  File::IOFile& stream = *handle->host_file;
  stream.Seek(handle->file_offset, File::SeekOrigin::Begin);
  if (!stream.WriteBytes(ptr, count))
    return std::unexpected{ResultCode::AccessDenied};

  handle->file_offset += count;
  return count;
}

Result<u32> HostFileSystem::SeekFile(Fd fd, std::uint32_t offset, SeekMode mode)
{
  Handle* handle = GetHandleFromFd(fd);
  if (handle == nullptr || !handle->host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  const u64 file_size = handle->host_file->GetSize();
  u32 target;
  if (mode == SeekMode::Set)
    target = offset;
  else if (mode == SeekMode::Current)
    target = handle->file_offset + offset;
  else if (mode == SeekMode::End)
    target = static_cast<u32>(file_size + offset);
  else
    return std::unexpected{ResultCode::Invalid};

  // Unlike POSIX, IOS refuses to move past the end of the file.
  if (target > file_size)
    return std::unexpected{ResultCode::Invalid};

  handle->file_offset = target;
  return target;
}

Result<FileStatus> HostFileSystem::GetFileStatus(Fd fd)
{
  const Handle* handle = GetHandleFromFd(fd);
  if (handle == nullptr || !handle->host_file->IsOpen())
    return std::unexpected{ResultCode::Invalid};

  FileStatus status;
  status.offset = handle->file_offset;
  status.size = static_cast<u32>(handle->host_file->GetSize());
  return status;
}

HostFileSystem::Handle* HostFileSystem::AssignFreeHandle()
{
  for (Handle& slot : m_handles)
  {
    if (slot.opened)
      continue;
    slot = Handle{};
    slot.opened = true;
    return &slot;
  }
  return nullptr;
}

HostFileSystem::Handle* HostFileSystem::GetHandleFromFd(Fd fd)
{
  if (fd < m_handles.size() && m_handles[fd].opened)
    return &m_handles[fd];
  return nullptr;
}

Fd HostFileSystem::ConvertHandleToFd(const Handle* handle) const
{
  return static_cast<Fd>(handle - m_handles.data());
}
}  // namespace IOS::HLE::FS
