// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/FS/FileSystemProxy.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"

namespace IOS::HLE
{
using namespace IOS::HLE::FS;

namespace
{
// ---------------------------------------------------------------------------------------------
// Timing model. All figures are timebase ticks measured on hardware for the FS module; the
// module was reworked in IOS28, and IOS28/IOS80 differ slightly from the other later versions.

bool IsIOS28Or80(u32 version)
{
  return version == 28 || version == 80;
}

// Writing the superblock (done after any metadata change and on close after a write).
SystemTimers::TimeBaseTick SuperblockWriteTime(u32 version)
{
  if (IsIOS28Or80(version))
    return 3350000_tbticks;
  return version < 28 ? 4100000_tbticks : 3170000_tbticks;
}

// One cluster written to NAND (half the measured time for a two-cluster write).
SystemTimers::TimeBaseTick ClusterWriteTime(u32 version)
{
  return version < 28 ? 370000_tbticks : 300000_tbticks;
}

// One cluster read from NAND.
SystemTimers::TimeBaseTick ClusterReadTime(u32 version)
{
  if (IsIOS28Or80(version))
    return 125000_tbticks;
  return version < 28 ? 165000_tbticks : 115000_tbticks;
}

// A cached access is a memcpy to or from the module's buffer. Linear fits of hardware
// measurements; old kernels copy byte by byte, newer ones in larger chunks.
SystemTimers::TimeBaseTick BufferCopyTime(u32 version, u64 bytes)
{
  const double n = static_cast<double>(bytes);
  const double tb = version < 28 ? 1.0 * n + 3.0 : 0.636 * n + 150.0;
  return SystemTimers::TimeBaseTick(static_cast<u64>(tb));
}

// Looking for a free cluster before a cached write.
constexpr SystemTimers::TimeBaseTick FREE_CLUSTER_CHECK_TIME = 1000_tbticks;

// Bytes of file data in one NAND cluster.
constexpr u32 CLUSTER_BYTES = 0x4000;

enum class LookupStyle
{
  // The whole path is walked once.
  Whole,
  // The parent and the last component are looked up separately.
  ParentThenName,
};

// Time to resolve a path in the FST. Every component is assumed to be found, even though a
// real lookup stops at the first missing one.
SystemTimers::TimeBaseTick PathLookupTime(const std::string& path, LookupStyle style)
{
  const auto components = static_cast<u64>(std::ranges::count(path, '/'));
  if (components == 0)
    return 0_tbticks;
  // A trailing slash is rejected before any lookup.
  if (path.back() == '/')
    return 300_tbticks;
  if (style == LookupStyle::Whole)
    return SystemTimers::TimeBaseTick(680 * components);
  return SystemTimers::TimeBaseTick(1000 + 340 * components);
}

IPCReply FSReply(s32 value, u64 extra_ticks = 0)
{
  return IPCReply{value, IPC_OVERHEAD_TICKS + extra_ticks};
}

IPCReply FSReply(ResultCode code, u64 extra_ticks = 0)
{
  return FSReply(ConvertResult(code), extra_ticks);
}

// Metadata changes are dominated by the superblock write, which only happens on success.
IPCReply SuperblockReply(u32 version, ResultCode code)
{
  return FSReply(code, code == ResultCode::Success ? u64{SuperblockWriteTime(version)} : 0);
}

// ---------------------------------------------------------------------------------------------
// Logging

void Report(ResultCode code, std::string_view what)
{
  const auto level =
      code == ResultCode::Success ? Common::Log::LogLevel::LINFO : Common::Log::LogLevel::LERROR;
  GENERIC_LOG_FMT(Common::Log::LogType::IOS_FS, level, "{} -> {}", what,
                  std::to_underlying(ConvertResult(code)));
}

template <typename T>
void Report(const Result<T>& result, std::string_view what)
{
  Report(result.has_value() ? ResultCode::Success : result.error(), what);
}

// ---------------------------------------------------------------------------------------------
// Request layouts

#pragma pack(push, 1)
// Used by CreateDir, CreateFile, SetAttr, SetFileVerCtrl and (as output) GetAttr.
struct ISFSParams
{
  Common::BigEndianValue<Uid> uid;
  Common::BigEndianValue<Gid> gid;
  char path[64];
  Modes modes;
  FileAttribute attribute;
};

struct ISFSNandStats
{
  Common::BigEndianValue<u32> cluster_size;
  Common::BigEndianValue<u32> free_clusters;
  Common::BigEndianValue<u32> used_clusters;
  Common::BigEndianValue<u32> bad_clusters;
  Common::BigEndianValue<u32> reserved_clusters;
  Common::BigEndianValue<u32> free_inodes;
  Common::BigEndianValue<u32> used_inodes;
};

struct ISFSFileStats
{
  Common::BigEndianValue<u32> size;
  Common::BigEndianValue<u32> seek_position;
};
#pragma pack(pop)

constexpr u32 PATH_FIELD = 64;

// Copies a request structure out of the input buffer, if the buffer is big enough.
template <typename T>
Result<T> ReadInput(Memory::MemoryManager& memory, const IOCtlRequest& request)
{
  if (request.buffer_in_size < sizeof(T))
    return std::unexpected{ResultCode::Invalid};

  T value;
  memory.CopyFromEmu(&value, request.buffer_in, sizeof(value));
  return value;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// FSCore

FSCore::FSCore(Kernel& ios) : m_ios(ios)
{
  // /tmp is wiped every time the FS module starts.
  const auto fs = ios.GetFS();
  if (fs->Delete(PID_KERNEL, PID_KERNEL, "/tmp") == ResultCode::Success)
  {
    fs->CreateDirectory(PID_KERNEL, PID_KERNEL, "/tmp", 0,
                        {Mode::ReadWrite, Mode::ReadWrite, Mode::ReadWrite});
  }
}

FSCore::~FSCore() = default;

FSCore::ScopedFd FSCore::Open(FS::Uid uid, FS::Gid gid, const std::string& path, FS::Mode mode,
                              std::optional<u32> ipc_fd, Ticks ticks)
{
  ticks.Add(IPC_OVERHEAD_TICKS);

  if (m_fd_map.size() >= 16)
    return {this, ConvertResult(ResultCode::NoFreeHandle), ticks};
  if (path.size() >= 64)
    return {this, ConvertResult(ResultCode::Invalid), ticks};

  // PPC opens use the kernel's descriptor number; internal opens get their own numbers.
  const u64 fd = ipc_fd ? u64{*ipc_fd} : m_next_fd++;

  // Opening the device itself gives a handle for ioctls only.
  if (path == "/dev/fs")
  {
    m_fd_map[fd] = {gid, uid, INVALID_FD};
    return {this, static_cast<s64>(fd), ticks};
  }

  ticks.Add(PathLookupTime(path, LookupStyle::Whole));

  Result<FileHandle> file = m_ios.GetFS()->OpenFile(uid, gid, path, mode);
  Report(file, fmt::format("OpenFile({})", path));
  if (!file)
    return {this, ConvertResult(file.error()), ticks};

  Handle& handle = m_fd_map[fd] = {gid, uid, file->Release()};
  std::strncpy(handle.name.data(), path.c_str(), handle.name.size());
  return {this, static_cast<s64>(fd), ticks};
}

s32 FSCore::Close(u64 fd, Ticks ticks)
{
  ticks.Add(IPC_OVERHEAD_TICKS);

  // Unknown descriptors are accepted (and leave no trace).
  const Handle& handle = m_fd_map[fd];
  if (handle.fs_fd == INVALID_FD)
  {
    m_fd_map.erase(fd);
    return IPC_SUCCESS;
  }

  // Closing the cached file writes the cache back first; a file that was written to then
  // needs a superblock update.
  if (m_cache_fd == fd)
  {
    ticks.Add(SimulateFlushFileCache());
    m_cache_fd.reset();
  }
  if (handle.superblock_flush_needed)
    ticks.Add(SuperblockWriteTime(m_ios.GetVersion()));

  const ResultCode result = m_ios.GetFS()->Close(handle.fs_fd);
  Report(result, fmt::format("Close({})", handle.name.data()));
  m_fd_map.erase(fd);
  return result == ResultCode::Success ? s32{IPC_SUCCESS} : s32{ConvertResult(result)};
}

bool FSCore::HasCacheForFile(u64 fd, u32 offset) const
{
  return m_cache_fd == fd && m_cache_chain_index == static_cast<u16>(offset / CLUSTER_BYTES);
}

// Loads the cluster holding `offset` into the cache (after writing back a dirty one).
u64 FSCore::SimulatePopulateFileCache(u64 fd, u32 offset, u32 file_size)
{
  if (HasCacheForFile(fd, offset))
    return 0;

  u64 ticks = SimulateFlushFileCache();
  // Nothing needs reading when the offset is the (cluster aligned) end of the file.
  const bool at_aligned_end = offset == file_size && offset % CLUSTER_BYTES == 0;
  if (offset < file_size && !at_aligned_end)
    ticks += ClusterReadTime(m_ios.GetVersion());

  m_cache_fd = fd;
  m_cache_chain_index = static_cast<u16>(offset / CLUSTER_BYTES);
  return ticks;
}

u64 FSCore::SimulateFlushFileCache()
{
  if (!m_dirty_cache || !m_cache_fd)
    return 0;
  m_dirty_cache = false;
  m_fd_map[*m_cache_fd].superblock_flush_needed = true;
  return ClusterWriteTime(m_ios.GetVersion());
}

// Walks a read or write cluster by cluster the way the FS module does, to work out its cost.
// Has to run before the data is actually transferred (it looks at the current position).
u64 FSCore::EstimateTicksForReadWrite(const Handle& handle, u64 fd, IPCCommandType command,
                                      u32 size)
{
  const u32 version = m_ios.GetVersion();
  const bool writing = command == IPC_CMD_WRITE;

  const Result<FileStatus> status = m_ios.GetFS()->GetFileStatus(handle.fs_fd);
  const u32 file_size = status->size;
  u32 position = status->offset;
  u32 remaining = size;
  // Reads stop at the end of the file (32-bit arithmetic, as in the module).
  if (!writing && remaining + position > file_size)
    remaining = file_size - position;

  u64 ticks = 0;
  while (remaining != 0)
  {
    u32 step;
    const bool whole_cluster = remaining >= CLUSTER_BYTES && position % CLUSTER_BYTES == 0;
    if (whole_cluster && !HasCacheForFile(fd, position))
    {
      // Aligned full clusters bypass the cache and go straight between NAND and the caller.
      ticks += writing ? ClusterWriteTime(version) : ClusterReadTime(version);
      if (writing)
        m_fd_map[fd].superblock_flush_needed = true;
      step = CLUSTER_BYTES;
    }
    else
    {
      ticks += SimulatePopulateFileCache(fd, position, file_size);

      const u32 within = position - u32{m_cache_chain_index} * CLUSTER_BYTES;
      step = std::min(CLUSTER_BYTES - within, remaining);
      ticks += BufferCopyTime(version, step);
      if (writing)
        ticks += FREE_CLUSTER_CHECK_TIME;
      m_dirty_cache = writing;

      // Filling the cached cluster to its end writes it back right away.
      if (writing && (position + step) % CLUSTER_BYTES == 0)
        ticks += SimulateFlushFileCache();
    }
    position += step;
    remaining -= step;
  }
  return ticks;
}

s32 FSCore::Read(u64 fd, u8* data, u32 size, std::optional<u32> ipc_buffer_addr, Ticks ticks)
{
  ticks.Add(IPC_OVERHEAD_TICKS);

  const Handle& handle = m_fd_map[fd];
  if (handle.fs_fd == INVALID_FD)
    return ConvertResult(ResultCode::Invalid);

  ticks.Add(EstimateTicksForReadWrite(handle, fd, IPC_CMD_READ, size));

  const Result<u32> result = m_ios.GetFS()->ReadBytesFromFile(handle.fs_fd, data, size);
  if (ipc_buffer_addr)
  {
    Report(result,
           fmt::format("Read({}, {:#010x}, {})", handle.name.data(), *ipc_buffer_addr, size));
  }
  if (!result)
    return ConvertResult(result.error());
  return static_cast<s32>(*result);
}

s32 FSCore::Write(u64 fd, const u8* data, u32 size, std::optional<u32> ipc_buffer_addr, Ticks ticks)
{
  ticks.Add(IPC_OVERHEAD_TICKS);

  const Handle& handle = m_fd_map[fd];
  if (handle.fs_fd == INVALID_FD)
    return ConvertResult(ResultCode::Invalid);

  ticks.Add(EstimateTicksForReadWrite(handle, fd, IPC_CMD_WRITE, size));

  const Result<u32> result = m_ios.GetFS()->WriteBytesToFile(handle.fs_fd, data, size);
  if (ipc_buffer_addr)
  {
    Report(result,
           fmt::format("Write({}, {:#010x}, {})", handle.name.data(), *ipc_buffer_addr, size));
  }
  if (!result)
    return ConvertResult(result.error());
  return static_cast<s32>(*result);
}

s32 FSCore::Seek(u64 fd, u32 offset, FS::SeekMode mode, Ticks ticks)
{
  ticks.Add(IPC_OVERHEAD_TICKS);

  const Handle& handle = m_fd_map[fd];
  if (handle.fs_fd == INVALID_FD)
    return ConvertResult(ResultCode::Invalid);

  const Result<u32> result = m_ios.GetFS()->SeekFile(handle.fs_fd, offset, mode);
  Report(result, fmt::format("Seek({}, {:#010x}, {})", handle.name.data(), offset,
                             static_cast<int>(mode)));
  if (!result)
    return ConvertResult(result.error());
  return static_cast<s32>(*result);
}

FS::Result<FS::FileStatus> FSCore::GetFileStatus(u64 fd, Ticks ticks)
{
  ticks.Add(IPC_OVERHEAD_TICKS);

  const Handle& handle = m_fd_map[fd];
  if (handle.fs_fd == INVALID_FD)
    return std::unexpected{ResultCode::Invalid};

  Result<FileStatus> status = m_ios.GetFS()->GetFileStatus(handle.fs_fd);
  Report(status, fmt::format("GetFileStatus({})", handle.name.data()));
  return status;
}

FS::ResultCode FSCore::DeleteFile(FS::Uid uid, FS::Gid gid, const std::string& path, Ticks ticks)
{
  ticks.Add(IPC_OVERHEAD_TICKS);
  const ResultCode result = m_ios.GetFS()->Delete(uid, gid, path);
  // Charged whether or not the delete worked.
  ticks.Add(SuperblockWriteTime(m_ios.GetVersion()));
  Report(result, fmt::format("Delete({})", path));
  return result;
}

FS::ResultCode FSCore::RenameFile(FS::Uid uid, FS::Gid gid, const std::string& old_path,
                                  const std::string& new_path, Ticks ticks)
{
  ticks.Add(IPC_OVERHEAD_TICKS);
  const ResultCode result = m_ios.GetFS()->Rename(uid, gid, old_path, new_path);
  ticks.Add(SuperblockWriteTime(m_ios.GetVersion()));
  Report(result, fmt::format("Rename({}, {})", old_path, new_path));
  return result;
}

FS::ResultCode FSCore::CreateFile(FS::Uid uid, FS::Gid gid, const std::string& path,
                                  FS::FileAttribute attribute, FS::Modes modes, Ticks ticks)
{
  ticks.Add(IPC_OVERHEAD_TICKS);
  const ResultCode result = m_ios.GetFS()->CreateFile(uid, gid, path, attribute, modes);
  ticks.Add(SuperblockWriteTime(m_ios.GetVersion()));
  Report(result, fmt::format("CreateFile({})", path));
  return result;
}

// ---------------------------------------------------------------------------------------------
// FSDevice

FSDevice::FSDevice(EmulationKernel& ios, FSCore& core, const std::string& device_name)
    : EmulationDevice(ios, device_name), m_core(core)
{
}

FSDevice::~FSDevice() = default;

void FSDevice::DoState(PointerWrap& p)
{
  Device::DoState(p);
  p.Do(m_core.m_dirty_cache);
  p.Do(m_core.m_cache_chain_index);
  p.Do(m_core.m_cache_fd);
  p.Do(m_core.m_next_fd);
  p.Do(m_core.m_fd_map);
}

std::optional<IPCReply> FSDevice::Open(const OpenRequest& request)
{
  const auto mode = static_cast<Mode>(request.flags & 3);
  return MakeIPCReply([&](Ticks t) {
    FSCore::ScopedFd fd = m_core.Open(request.uid, request.gid, request.path, mode, request.fd, t);
    return static_cast<s32>(fd.Release());
  });
}

std::optional<IPCReply> FSDevice::Close(u32 fd)
{
  return MakeIPCReply([&](Ticks t) { return m_core.Close(u64{fd}, t); });
}

std::optional<IPCReply> FSDevice::Read(const ReadWriteRequest& request)
{
  return MakeIPCReply([&](Ticks t) {
    u8* const buffer = GetSystem().GetMemory().GetPointerForRange(request.buffer, request.size);
    return m_core.Read(request.fd, buffer, request.size, request.buffer, t);
  });
}

std::optional<IPCReply> FSDevice::Write(const ReadWriteRequest& request)
{
  return MakeIPCReply([&](Ticks t) {
    const u8* const buffer =
        GetSystem().GetMemory().GetPointerForRange(request.buffer, request.size);
    return m_core.Write(request.fd, buffer, request.size, request.buffer, t);
  });
}

std::optional<IPCReply> FSDevice::Seek(const SeekRequest& request)
{
  return MakeIPCReply([&](Ticks t) {
    return m_core.Seek(request.fd, request.offset, static_cast<FS::SeekMode>(request.mode), t);
  });
}

std::optional<IPCReply> FSDevice::IOCtl(const IOCtlRequest& request)
{
  const auto entry = m_core.m_fd_map.find(request.fd);
  if (entry == m_core.m_fd_map.end())
    return IPCReply(ConvertResult(ResultCode::Invalid));
  const Handle& handle = entry->second;

  switch (request.request)
  {
  case ISFS_IOCTL_FORMAT:
    return Format(handle, request);
  case ISFS_IOCTL_GETSTATS:
    return GetStats(handle, request);
  case ISFS_IOCTL_CREATEDIR:
    return CreateDirectory(handle, request);
  case ISFS_IOCTL_SETATTR:
    return SetAttribute(handle, request);
  case ISFS_IOCTL_GETATTR:
    return GetAttribute(handle, request);
  case ISFS_IOCTL_DELETE:
    return DeleteFile(handle, request);
  case ISFS_IOCTL_RENAME:
    return RenameFile(handle, request);
  case ISFS_IOCTL_CREATEFILE:
    return CreateFile(handle, request);
  case ISFS_IOCTL_SETFILEVERCTRL:
    return SetFileVersionControl(handle, request);
  case ISFS_IOCTL_GETFILESTATS:
    return GetFileStats(handle, request);
  case ISFS_IOCTL_SHUTDOWN:
    return Shutdown(handle, request);
  default:
    return FSReply(ResultCode::Invalid);
  }
}

std::optional<IPCReply> FSDevice::IOCtlV(const IOCtlVRequest& request)
{
  const auto entry = m_core.m_fd_map.find(request.fd);
  if (entry == m_core.m_fd_map.end())
    return IPCReply(ConvertResult(ResultCode::Invalid));

  if (request.request == ISFS_IOCTLV_READDIR)
    return ReadDirectory(entry->second, request);
  if (request.request == ISFS_IOCTLV_GETUSAGE)
    return GetUsage(entry->second, request);
  return FSReply(ResultCode::Invalid);
}

IPCReply FSDevice::Format(const Handle& handle, const IOCtlRequest&)
{
  if (handle.uid != 0)
    return FSReply(ResultCode::AccessDenied);

  return SuperblockReply(m_ios.GetVersion(), m_ios.GetFS()->Format(handle.uid));
}

IPCReply FSDevice::GetStats(const Handle&, const IOCtlRequest& request)
{
  if (request.buffer_out_size < sizeof(ISFSNandStats))
    return FSReply(ResultCode::Invalid);

  const Result<NandStats> stats = m_ios.GetFS()->GetNandStats();
  Report(stats, "GetNandStats");
  if (!stats)
    return IPCReply(ConvertResult(stats.error()));

  ISFSNandStats out;
  out.cluster_size = stats->cluster_size;
  out.free_clusters = stats->free_clusters;
  out.used_clusters = stats->used_clusters;
  out.bad_clusters = stats->bad_clusters;
  out.reserved_clusters = stats->reserved_clusters;
  out.free_inodes = stats->free_inodes;
  out.used_inodes = stats->used_inodes;
  GetSystem().GetMemory().CopyToEmu(request.buffer_out, &out, sizeof(out));
  return IPCReply(IPC_SUCCESS);
}

IPCReply FSDevice::CreateDirectory(const Handle& handle, const IOCtlRequest& request)
{
  const auto params = ReadInput<ISFSParams>(GetSystem().GetMemory(), request);
  if (!params)
    return FSReply(params.error());

  const ResultCode result = m_ios.GetFS()->CreateDirectory(handle.uid, handle.gid, params->path,
                                                           params->attribute, params->modes);
  Report(result, fmt::format("CreateDirectory({})", params->path));
  return SuperblockReply(m_ios.GetVersion(), result);
}

// Two forms:
//   in[0] = path (64 bytes), io[0] = u32 count            -> just the number of entries;
//   in[0] = path, in[1] = u32 max, io[0] = names (13*max), io[1] = u32 count
//                                                         -> up to max NUL-separated names.
IPCReply FSDevice::ReadDirectory(const Handle& handle, const IOCtlVRequest& request)
{
  const size_t vectors = request.in_vectors.size();
  if (vectors == 0 || vectors > 2 || vectors != request.io_vectors.size() ||
      request.in_vectors[0].size != PATH_FIELD)
  {
    return FSReply(ResultCode::Invalid);
  }

  auto& memory = GetSystem().GetMemory();

  u32 names_address = 0;
  u32 count_address;
  u32 max_entries;
  if (vectors == 2)
  {
    if (request.in_vectors[1].size != 4 || request.io_vectors[1].size != 4)
      return FSReply(ResultCode::Invalid);
    max_entries = memory.Read_U32(request.in_vectors[1].address);
    names_address = request.io_vectors[0].address;
    count_address = request.io_vectors[1].address;
    // 13 bytes per name (12 characters and a NUL); the product wraps like a 32-bit multiply.
    if (request.io_vectors[0].size != 13 * max_entries)
      return FSReply(ResultCode::Invalid);
    memory.Write_U32(max_entries, count_address);
  }
  else
  {
    if (request.io_vectors[0].size != 4)
      return FSReply(ResultCode::Invalid);
    count_address = request.io_vectors[0].address;
    max_entries = memory.Read_U32(count_address);
  }

  const std::string directory = memory.GetString(request.in_vectors[0].address, PATH_FIELD);
  const Result<std::vector<std::string>> names =
      m_ios.GetFS()->ReadDirectory(handle.uid, handle.gid, directory);
  Report(names, fmt::format("ReadDirectory({})", directory));
  if (!names)
    return FSReply(names.error());

  const auto total = static_cast<u32>(names->size());
  if (names_address == 0)
  {
    memory.Write_U32(total, count_address);
    return FSReply(IPC_SUCCESS);
  }

  // Names are packed one after the other, each followed by a NUL. Each write also clears the
  // 13 bytes from the start of the name.
  u32 cursor = names_address;
  const u32 written = std::min(max_entries, total);
  for (u32 i = 0; i < written; ++i)
  {
    const std::string& name = (*names)[i];
    memory.Memset(cursor, 0, 13);
    memory.CopyToEmu(cursor, name.data(), name.size());
    memory.Write_U8(0, cursor + 12);
    cursor += static_cast<u32>(name.size()) + 1;
  }
  memory.Write_U32(written, count_address);
  return FSReply(IPC_SUCCESS);
}

IPCReply FSDevice::SetAttribute(const Handle& handle, const IOCtlRequest& request)
{
  const auto params = ReadInput<ISFSParams>(GetSystem().GetMemory(), request);
  if (!params)
    return FSReply(params.error());

  const ResultCode result = m_ios.GetFS()->SetMetadata(
      handle.uid, params->path, params->uid, params->gid, params->attribute, params->modes);
  Report(result, fmt::format("SetMetadata({})", params->path));
  return SuperblockReply(m_ios.GetVersion(), result);
}

IPCReply FSDevice::GetAttribute(const Handle& handle, const IOCtlRequest& request)
{
  if (request.buffer_in_size < PATH_FIELD || request.buffer_out_size < sizeof(ISFSParams))
    return FSReply(ResultCode::Invalid);

  auto& memory = GetSystem().GetMemory();
  const std::string path = memory.GetString(request.buffer_in, PATH_FIELD);
  const u64 lookup = PathLookupTime(path, LookupStyle::ParentThenName);

  const Result<Metadata> metadata = m_ios.GetFS()->GetMetadata(handle.uid, handle.gid, path);
  Report(metadata, fmt::format("GetMetadata({})", path));
  if (!metadata)
    return FSReply(metadata.error(), lookup);

  // Only the owner, group, attribute and modes are filled in. IOS leaves the rest of the
  // structure as whatever was on its stack; here it is zero so that results are reproducible.
  ISFSParams out{};
  out.uid = metadata->uid;
  out.gid = metadata->gid;
  out.attribute = metadata->attribute;
  out.modes = metadata->modes;
  memory.CopyToEmu(request.buffer_out, &out, sizeof(out));
  return FSReply(IPC_SUCCESS, lookup);
}

IPCReply FSDevice::DeleteFile(const Handle& handle, const IOCtlRequest& request)
{
  if (request.buffer_in_size < PATH_FIELD)
    return FSReply(ResultCode::Invalid);

  const std::string path = GetSystem().GetMemory().GetString(request.buffer_in, PATH_FIELD);
  return MakeIPCReply([&](Ticks t) {
    return ConvertResult(m_core.DeleteFile(handle.uid, handle.gid, path, t));
  });
}

IPCReply FSDevice::RenameFile(const Handle& handle, const IOCtlRequest& request)
{
  if (request.buffer_in_size < 2 * PATH_FIELD)
    return FSReply(ResultCode::Invalid);

  auto& memory = GetSystem().GetMemory();
  const std::string from = memory.GetString(request.buffer_in, PATH_FIELD);
  const std::string to = memory.GetString(request.buffer_in + PATH_FIELD, PATH_FIELD);
  return MakeIPCReply([&](Ticks t) {
    return ConvertResult(m_core.RenameFile(handle.uid, handle.gid, from, to, t));
  });
}

IPCReply FSDevice::CreateFile(const Handle& handle, const IOCtlRequest& request)
{
  const auto params = ReadInput<ISFSParams>(GetSystem().GetMemory(), request);
  if (!params)
    return FSReply(params.error());

  // The core's timing is deliberately not collected here, so this reply has no delay at all.
  return MakeIPCReply([&](Ticks) {
    return ConvertResult(
        m_core.CreateFile(handle.uid, handle.gid, params->path, params->attribute, params->modes));
  });
}

IPCReply FSDevice::SetFileVersionControl(const Handle&, const IOCtlRequest& request)
{
  const auto params = ReadInput<ISFSParams>(GetSystem().GetMemory(), request);
  if (!params)
    return FSReply(params.error());

  // Not implemented; reported as done.
  ERROR_LOG_FMT(IOS_FS, "SetFileVersionControl({}, {:#x}) is not implemented", params->path,
                params->attribute);
  return FSReply(IPC_SUCCESS);
}

IPCReply FSDevice::GetFileStats(const Handle&, const IOCtlRequest& request)
{
  if (request.buffer_out_size < sizeof(ISFSFileStats))
    return FSReply(ResultCode::Invalid);

  return MakeIPCReply([&](Ticks t) -> s32 {
    const Result<FileStatus> status = m_core.GetFileStatus(request.fd, t);
    if (!status)
      return ConvertResult(status.error());

    ISFSFileStats out;
    out.size = status->size;
    out.seek_position = status->offset;
    GetSystem().GetMemory().CopyToEmu(request.buffer_out, &out, sizeof(out));
    return IPC_SUCCESS;
  });
}

IPCReply FSDevice::GetUsage(const Handle&, const IOCtlVRequest& request)
{
  const bool shape_ok = request.HasNumberOfValidVectors(1, 2) &&
                        request.in_vectors[0].size == PATH_FIELD &&
                        request.io_vectors[0].size == 4 && request.io_vectors[1].size == 4;
  if (!shape_ok)
    return FSReply(ResultCode::Invalid);

  auto& memory = GetSystem().GetMemory();
  const std::string directory = memory.GetString(request.in_vectors[0].address, PATH_FIELD);
  const Result<DirectoryStats> stats = m_ios.GetFS()->GetDirectoryStats(directory);
  Report(stats, fmt::format("GetDirectoryStats({})", directory));
  if (!stats)
    return FSReply(stats.error());

  memory.Write_U32(stats->used_clusters, request.io_vectors[0].address);
  memory.Write_U32(stats->used_inodes, request.io_vectors[1].address);
  return FSReply(IPC_SUCCESS);
}

IPCReply FSDevice::Shutdown(const Handle&, const IOCtlRequest&)
{
  INFO_LOG_FMT(IOS_FS, "Shutdown requested");
  return FSReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
