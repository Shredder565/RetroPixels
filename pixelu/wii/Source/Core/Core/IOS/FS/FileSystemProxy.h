// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"

class PointerWrap;

namespace IOS::HLE
{
constexpr FS::Fd INVALID_FD = 0xffffffff;

class FSDevice;

// The FS module's request handling, shared by the /dev/fs device and by kernel code (ES, boot)
// that reads the NAND. It keeps per-descriptor state and models how long each operation takes,
// including the effect of the module's one-cluster cache.
class FSCore final
{
public:
  explicit FSCore(Kernel& ios);
  FSCore(const FSCore& other) = delete;
  FSCore(FSCore&& other) = delete;
  FSCore& operator=(const FSCore& other) = delete;
  FSCore& operator=(FSCore&& other) = delete;
  ~FSCore();

  // A descriptor (or negative error) that is closed again when it goes out of scope.
  class ScopedFd
  {
  public:
    ScopedFd(FSCore* fs, s64 fd, Ticks tick_tracker = {})
        : m_fs{fs}, m_fd{fd}, m_tick_tracker{tick_tracker}
    {
    }

    ~ScopedFd()
    {
      if (m_fd >= 0)
        m_fs->Close(static_cast<u64>(m_fd), m_tick_tracker);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd& operator=(ScopedFd&&) = delete;

    s64 Get() const { return m_fd; }
    s64 Release() { return std::exchange(m_fd, -1); }

  private:
    FSCore* m_fs{};
    s64 m_fd = -1;
    Ticks m_tick_tracker{};
  };

  // Counterparts of the IPC commands. Their tick counts include the IPC overhead.
  ScopedFd Open(FS::Uid uid, FS::Gid gid, const std::string& path, FS::Mode mode,
                std::optional<u32> ipc_fd = {}, Ticks ticks = {});
  s32 Close(u64 fd, Ticks ticks = {});
  s32 Read(u64 fd, u8* data, u32 size, std::optional<u32> ipc_buffer_addr = {}, Ticks ticks = {});
  s32 Write(u64 fd, const u8* data, u32 size, std::optional<u32> ipc_buffer_addr = {},
            Ticks ticks = {});
  s32 Seek(u64 fd, u32 offset, FS::SeekMode mode, Ticks ticks = {});

  FS::Result<FS::FileStatus> GetFileStatus(u64 fd, Ticks ticks = {});
  FS::ResultCode RenameFile(FS::Uid uid, FS::Gid gid, const std::string& old_path,
                            const std::string& new_path, Ticks ticks = {});
  FS::ResultCode DeleteFile(FS::Uid uid, FS::Gid gid, const std::string& path, Ticks ticks = {});
  FS::ResultCode CreateFile(FS::Uid uid, FS::Gid gid, const std::string& path,
                            FS::FileAttribute attribute, FS::Modes modes, Ticks ticks = {});

  template <typename T>
  s32 Read(u64 fd, T* data, size_t count, Ticks ticks = {})
  {
    const auto bytes = static_cast<u32>(sizeof(T) * count);
    return Read(fd, reinterpret_cast<u8*>(data), bytes, {}, ticks);
  }

  std::shared_ptr<FS::FileSystem> GetFS() const { return m_ios.GetFS(); }

private:
  // Saved as raw bytes in the savestate, so its layout must not change.
  struct Handle
  {
    u16 gid = 0;
    u32 uid = 0;
    FS::Fd fs_fd = INVALID_FD;
    std::array<char, 64> name{};
    bool superblock_flush_needed = false;
  };

  u64 EstimateTicksForReadWrite(const Handle& handle, u64 fd, IPCCommandType command, u32 size);
  u64 SimulatePopulateFileCache(u64 fd, u32 offset, u32 file_size);
  u64 SimulateFlushFileCache();
  bool HasCacheForFile(u64 fd, u32 offset) const;

  Kernel& m_ios;

  // The cache holds one cluster (m_cache_chain_index) of one file (m_cache_fd).
  bool m_dirty_cache = false;
  u16 m_cache_chain_index = 0;
  std::optional<u64> m_cache_fd;
  // Descriptors below 0x18 are the PPC's; internal opens are numbered from here.
  u64 m_next_fd = 0x18;
  std::map<u64, Handle> m_fd_map;

  friend class FSDevice;
};

// /dev/fs as seen through IPC. Opening any other absolute path also lands here.
class FSDevice final : public EmulationDevice
{
public:
  FSDevice(EmulationKernel& ios, FSCore& core, const std::string& device_name);
  ~FSDevice() override;

  void DoState(PointerWrap& p) override;

  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> Close(u32 fd) override;
  std::optional<IPCReply> Read(const ReadWriteRequest& request) override;
  std::optional<IPCReply> Write(const ReadWriteRequest& request) override;
  std::optional<IPCReply> Seek(const SeekRequest& request) override;
  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

private:
  enum
  {
    ISFS_IOCTL_FORMAT = 1,
    ISFS_IOCTL_GETSTATS = 2,
    ISFS_IOCTL_CREATEDIR = 3,
    ISFS_IOCTLV_READDIR = 4,
    ISFS_IOCTL_SETATTR = 5,
    ISFS_IOCTL_GETATTR = 6,
    ISFS_IOCTL_DELETE = 7,
    ISFS_IOCTL_RENAME = 8,
    ISFS_IOCTL_CREATEFILE = 9,
    ISFS_IOCTL_SETFILEVERCTRL = 10,
    ISFS_IOCTL_GETFILESTATS = 11,
    ISFS_IOCTLV_GETUSAGE = 12,
    ISFS_IOCTL_SHUTDOWN = 13,
  };

  using Handle = FSCore::Handle;

  IPCReply Format(const Handle& handle, const IOCtlRequest& request);
  IPCReply GetStats(const Handle& handle, const IOCtlRequest& request);
  IPCReply CreateDirectory(const Handle& handle, const IOCtlRequest& request);
  IPCReply ReadDirectory(const Handle& handle, const IOCtlVRequest& request);
  IPCReply SetAttribute(const Handle& handle, const IOCtlRequest& request);
  IPCReply GetAttribute(const Handle& handle, const IOCtlRequest& request);
  IPCReply DeleteFile(const Handle& handle, const IOCtlRequest& request);
  IPCReply RenameFile(const Handle& handle, const IOCtlRequest& request);
  IPCReply CreateFile(const Handle& handle, const IOCtlRequest& request);
  IPCReply SetFileVersionControl(const Handle& handle, const IOCtlRequest& request);
  IPCReply GetFileStats(const Handle& handle, const IOCtlRequest& request);
  IPCReply GetUsage(const Handle& handle, const IOCtlVRequest& request);
  IPCReply Shutdown(const Handle& handle, const IOCtlRequest& request);

  FSCore& m_core;
};
}  // namespace IOS::HLE
