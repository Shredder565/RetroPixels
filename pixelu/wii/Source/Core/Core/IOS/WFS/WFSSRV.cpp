// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// WFS file server. Only what the vWii title installer needs is implemented; unknown requests
// succeed with a cleared output buffer.

#include "Core/IOS/WFS/WFSSRV.h"

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/StringUtil.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace WFS
{
std::string NativePath(const std::string& wfs_path)
{
  return File::GetUserPath(D_WFSROOT_IDX) + Common::EscapePath(wfs_path);
}
}  // namespace WFS

namespace
{
// Strings in requests are a big-endian u16 length followed by the characters.
std::string ReadPrefixedString(Memory::MemoryManager& memory, u32 address)
{
  const u16 length = memory.Read_U16(address);
  return memory.GetString(address + 2, length);
}

// fopen modes for the WFS open modes (1: read, 2: write, 3: both).
const char* HostOpenMode(int mode)
{
  switch (mode)
  {
  case 1:
    return "rb";
  case 2:
    return "wb";
  case 3:
    return "rwb";
  default:
    return nullptr;
  }
}

// Reported size of the storage device.
constexpr u64 DEVICE_CAPACITY = u64{16} << 30;
}  // namespace

WFSSRVDevice::WFSSRVDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name), m_device_name("msc01")
{
}

std::optional<IPCReply> WFSSRVDevice::IOCtl(const IOCtlRequest& request)
{
  Core::System& system = GetSystem();
  Memory::MemoryManager& memory = system.GetMemory();
  const u32 in = request.buffer_in;
  const u32 out = request.buffer_out;
  s32 result = IPC_SUCCESS;

  switch (request.request)
  {
  case IOCTL_WFS_INIT:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_INIT (not implemented)");
    break;

  case IOCTL_WFS_UNKNOWN_8:
  {
    // Purpose unknown; the installer is happy with a length-prefixed device name.
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_UNKNOWN_8");
    static constexpr char NAME[8] = {'m', 's', 'c', '0', '1', 0, 0, 0};
    memory.Write_U8(7, out);
    memory.CopyToEmu(out + 1, NAME, sizeof(NAME));
    break;
  }

  case IOCTL_WFS_SHUTDOWN:
    // Answer the attach/detach requests that were left waiting.
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_SHUTDOWN");
    for (const u32 address : m_hanging)
    {
      const IOCtlRequest waiting{system, address};
      const u32 waiting_out = waiting.buffer_out;
      memory.Write_U32(0x80000000, waiting_out);
      memory.Write_U32(0, waiting_out + 4);
      memory.Write_U32(0, waiting_out + 8);
      GetEmulationKernel().EnqueueIPCReply(waiting, 0);
    }
    break;

  case IOCTL_WFS_DEVICE_INFO:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_DEVICE_INFO");
    memory.Write_U64(DEVICE_CAPACITY, out);
    memory.Write_U8(4, out + 8);
    break;

  case IOCTL_WFS_GET_DEVICE_NAME:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_GET_DEVICE_NAME");
    memory.Write_U8(static_cast<u8>(m_device_name.size()), out);
    memory.CopyToEmu(out + 1, m_device_name.data(), m_device_name.size());
    break;

  case IOCTL_WFS_ATTACH_DETACH_2:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_ATTACH_DETACH_2 (not implemented)");
    memory.Write_U32(1, out);
    memory.Write_U32(0, out + 4);
    memory.Write_U32(0, out + 8);
    break;

  case IOCTL_WFS_ATTACH_DETACH:
    // Never answered until shutdown.
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_ATTACH_DETACH: holding the request");
    m_hanging.push_back(request.address);
    return std::nullopt;

  case IOCTL_WFS_FLUSH:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_FLUSH (nothing to do)");
    break;

  case IOCTL_WFS_MKDIR:
  {
    const std::string path = NormalizePath(ReadPrefixedString(memory, in + 32));
    const std::string host_path = WFS::NativePath(path);
    if (File::Exists(host_path))
      result = WFS_EEXIST;
    else if (!File::CreateDir(host_path))
      result = WFS_ENOENT;
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_MKDIR({}) -> {}", path, result);
    break;
  }

  // Directory listing is faked: the only glob the installer does lists /vol/*, which yields the
  // one device.
  case IOCTL_WFS_GLOB_START:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_GLOB_START");
    memory.Memset(out, 0, request.buffer_out_size);
    memory.CopyToEmu(out + 0x14, m_device_name.data(), m_device_name.size());
    break;

  case IOCTL_WFS_GLOB_NEXT:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_GLOB_NEXT");
    result = WFS_ENOENT;
    break;

  case IOCTL_WFS_GLOB_END:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_GLOB_END");
    memory.Memset(out, 0, request.buffer_out_size);
    break;

  case IOCTL_WFS_SET_HOMEDIR:
    m_home_directory = ReadPrefixedString(memory, in);
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_SET_HOMEDIR: {}", m_home_directory);
    break;

  case IOCTL_WFS_CHDIR:
    m_current_directory = ReadPrefixedString(memory, in);
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_CHDIR: {}", m_current_directory);
    break;

  case IOCTL_WFS_GET_HOMEDIR:
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_GET_HOMEDIR: {}", m_home_directory);
    memory.Write_U16(static_cast<u16>(m_home_directory.size()), out);
    memory.CopyToEmu(out + 2, m_home_directory.data(), m_home_directory.size());
    break;

  case IOCTL_WFS_GET_ATTRIBUTES:
  {
    const std::string path = NormalizePath(ReadPrefixedString(memory, in));
    const std::string host_path = WFS::NativePath(path);

    // Note the argument order: this fills guest memory from address 0 with the low byte of the
    // output address, rather than clearing the output. Kept as is.
    memory.Memset(0, static_cast<u8>(out), request.buffer_out_size);

    if (!File::Exists(host_path))
    {
      INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_GET_ATTRIBUTES({}): not found", path);
      result = WFS_ENOENT;
    }
    else if (File::IsDirectory(host_path))
    {
      INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_GET_ATTRIBUTES({}): directory", path);
      memory.Write_U32(0x80000000, out + 4);
    }
    else
    {
      const auto size = static_cast<u32>(File::GetSize(host_path));
      INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_GET_ATTRIBUTES({}): file of {} bytes", path, size);
      memory.Write_U32(size, out);
    }
    break;
  }

  case IOCTL_WFS_RENAME:
  case IOCTL_WFS_RENAME_2:
  {
    // Two 512-byte path slots.
    std::string from = ReadPrefixedString(memory, in);
    std::string to = ReadPrefixedString(memory, in + 512);
    result = Rename(std::move(from), std::move(to));
    break;
  }

  case IOCTL_WFS_CREATE_OPEN:
  case IOCTL_WFS_OPEN:
  {
    // CREATE_OPEN always opens for writing; OPEN takes the mode from the request.
    const bool plain_open = request.request == IOCTL_WFS_OPEN;
    const u32 mode = plain_open ? memory.Read_U32(in) : 2;
    const std::string path = NormalizePath(ReadPrefixedString(memory, in + 0x20));
    const char* name = plain_open ? "IOCTL_WFS_OPEN" : "IOCTL_WFS_CREATE_OPEN";

    const u16 fd = GetNewFileDescriptor();
    FileDescriptor& entry = m_fds[fd];
    entry.in_use = true;
    entry.path = path;
    entry.mode = static_cast<int>(mode);
    entry.position = 0;
    if (!entry.Open())
    {
      ERROR_LOG_FMT(IOS_WFS, "{}({}, {}): cannot open the file", name, path, mode);
      ReleaseFileDescriptor(fd);
      result = WFS_ENOENT;
      break;
    }

    INFO_LOG_FMT(IOS_WFS, "{}({}, {}) -> fd {}", name, path, mode, fd);
    memory.Write_U16(fd, plain_open ? out + 0x14 : out);
    break;
  }

  case IOCTL_WFS_GET_SIZE:
  {
    const u16 fd = memory.Read_U16(in);
    FileDescriptor* entry = FindFileDescriptor(fd);
    if (entry == nullptr)
    {
      ERROR_LOG_FMT(IOS_WFS, "IOCTL_WFS_GET_SIZE: bad fd {}", fd);
      result = WFS_EBADFD;
      break;
    }

    // The reply only has room for 32 bits.
    const u64 size = entry->file.GetSize();
    const auto reported = static_cast<u32>(size);
    if (reported != size)
      ERROR_LOG_FMT(IOS_WFS, "IOCTL_WFS_GET_SIZE: fd {} is {} bytes, too large", fd, size);
    INFO_LOG_FMT(IOS_WFS, "IOCTL_WFS_GET_SIZE({}) -> {}", fd, reported);
    memory.Write_U32(reported, out);
    break;
  }

  case IOCTL_WFS_CLOSE:
  case IOCTL_WFS_CLOSE_2:
  {
    // How the two closes differ is unknown; both just release the descriptor.
    const u16 fd = memory.Read_U16(in + 4);
    INFO_LOG_FMT(IOS_WFS, "Close fd {} (ioctl {:#x})", fd, request.request);
    ReleaseFileDescriptor(fd);
    break;
  }

  case IOCTL_WFS_READ:
  case IOCTL_WFS_READ_ABSOLUTE:
  case IOCTL_WFS_WRITE:
  case IOCTL_WFS_WRITE_ABSOLUTE:
  {
    // in: data address, file offset (absolute variants only), length, fd.
    const bool writing =
        request.request == IOCTL_WFS_WRITE || request.request == IOCTL_WFS_WRITE_ABSOLUTE;
    const bool absolute =
        request.request == IOCTL_WFS_READ_ABSOLUTE || request.request == IOCTL_WFS_WRITE_ABSOLUTE;
    const u32 data_address = memory.Read_U32(in);
    const u32 offset = memory.Read_U32(in + 4);
    const u32 length = memory.Read_U32(in + 8);
    const u16 fd = memory.Read_U16(in + 0xC);
    const char* name = writing ? "IOCTL_WFS_WRITE" : "IOCTL_WFS_READ";

    FileDescriptor* entry = FindFileDescriptor(fd);
    if (entry == nullptr)
    {
      ERROR_LOG_FMT(IOS_WFS, "{}: bad fd {}", name, fd);
      result = WFS_EBADFD;
      break;
    }

    // Absolute accesses leave the file position where it was. I/O errors are not reported.
    const u64 saved_position = entry->file.Tell();
    if (absolute)
      entry->file.Seek(offset, File::SeekOrigin::Begin);

    u8* data = memory.GetPointerForRange(data_address, length);
    size_t transferred = length;
    if (writing)
      entry->file.WriteArray(data, length);
    else
      entry->file.ReadArray(data, length, &transferred);

    if (absolute)
      entry->file.Seek(static_cast<s64>(saved_position), File::SeekOrigin::Begin);
    else
      entry->position += transferred;

    INFO_LOG_FMT(IOS_WFS, "{}: {} bytes, fd {} ({})", name, transferred, fd, entry->path);
    // Reads return the byte count; writes return success.
    if (!writing)
      result = static_cast<s32>(transferred);
    break;
  }

  default:
    // Pretend unknown requests worked; returning errors would break the installer early.
    request.DumpUnknown(system, GetDeviceName(), Common::Log::LogType::IOS_WFS,
                        Common::Log::LogLevel::LWARNING);
    memory.Memset(out, 0, request.buffer_out_size);
    break;
  }

  return IPCReply(result);
}

s32 WFSSRVDevice::Rename(std::string source, std::string dest) const
{
  const std::string from = NormalizePath(source);
  const std::string to = NormalizePath(dest);
  INFO_LOG_FMT(IOS_WFS, "Rename {} -> {}", from, to);

  const auto is_open = [&from](const FileDescriptor& d) { return d.in_use && d.path == from; };
  if (std::ranges::any_of(m_fds, is_open))
    return WFS_FILE_IS_OPENED;

  // Every host failure is reported as "not found".
  if (!File::Rename(WFS::NativePath(from), WFS::NativePath(to)))
    return WFS_ENOENT;
  return IPC_SUCCESS;
}

void WFSSRVDevice::SetHomeDir(const std::string& home_dir)
{
  m_home_directory = home_dir;
}

std::string WFSSRVDevice::NormalizePath(const std::string& path) const
{
  std::string absolute;
  if (path.starts_with('~'))
    absolute = m_home_directory + '/' + path.substr(1);
  else if (path.starts_with('/'))
    absolute = path;
  else
    absolute = m_current_directory + '/' + path;

  // ".." above the root stays in the path as a name.
  std::vector<std::string> parts;
  for (std::string& part : SplitString(absolute, '/'))
  {
    if (part.empty() || part == ".")
      continue;
    if (part == ".." && !parts.empty())
      parts.pop_back();
    else
      parts.push_back(std::move(part));
  }

  if (parts.empty())
    return "/";
  std::string normalized;
  for (const std::string& part : parts)
  {
    normalized += '/';
    normalized += part;
  }
  return normalized;
}

WFSSRVDevice::FileDescriptor* WFSSRVDevice::FindFileDescriptor(u16 fd)
{
  if (fd < m_fds.size() && m_fds[fd].in_use)
    return &m_fds[fd];
  return nullptr;
}

u16 WFSSRVDevice::GetNewFileDescriptor()
{
  const auto free_slot =
      std::ranges::find_if(m_fds, [](const FileDescriptor& d) { return !d.in_use; });
  if (free_slot == m_fds.end())
  {
    m_fds.emplace_back();
    return static_cast<u16>(m_fds.size() - 1);
  }
  return static_cast<u16>(free_slot - m_fds.begin());
}

void WFSSRVDevice::ReleaseFileDescriptor(u16 fd)
{
  FileDescriptor* entry = FindFileDescriptor(fd);
  if (entry == nullptr)
    return;
  entry->in_use = false;

  // Drop unused entries from the end so the table does not grow forever.
  while (!m_fds.empty() && !m_fds.back().in_use)
    m_fds.pop_back();
}

bool WFSSRVDevice::FileDescriptor::Open()
{
  const char* host_mode = HostOpenMode(mode);
  if (host_mode == nullptr)
  {
    ERROR_LOG_FMT(IOS_WFS, "WFS open: unsupported mode {}", mode);
    return false;
  }
  return file.Open(WFS::NativePath(path), host_mode);
}
}  // namespace IOS::HLE
