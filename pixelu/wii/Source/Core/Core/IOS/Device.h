// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/IOS/IOS.h"

namespace Core
{
class System;
}

namespace IOS::HLE
{
// Result codes that IOS writes into the reply word of an IPC request. The groups follow the
// module that produces them (kernel, FS, ES, IOSC, USB); see WiiBrew "IOS/Error codes".
enum ReturnCode : s32
{
  // Kernel / generic
  IPC_SUCCESS = 0,
  IPC_EACCES = -1,
  IPC_EEXIST = -2,
  IPC_STALL = -3,
  IPC_EINVAL = -4,
  IPC_EMAX = -5,
  IPC_ENOENT = -6,
  IPC_EQUEUEFULL = -8,
  IPC_UNKNOWN = -9,
  IPC_EIO = -12,
  IPC_ENOMEM = -22,
  // File system
  FS_EINVAL = -101,
  FS_EACCESS = -102,
  FS_ECORRUPT = -103,
  FS_EEXIST = -105,
  FS_ENOENT = -106,
  FS_ENFILE = -107,
  FS_EFBIG = -108,
  FS_EFDEXHAUSTED = -109,
  FS_ENAMELEN = -110,
  FS_EFDOPEN = -111,
  FS_EIO = -114,
  FS_ENOTEMPTY = -115,
  FS_EDIRDEPTH = -116,
  FS_EBUSY = -118,
  // ES
  ES_SHORT_READ = -1009,
  ES_EIO = -1010,
  ES_INVALID_SIGNATURE_TYPE = -1012,
  ES_FD_EXHAUSTED = -1016,
  ES_EINVAL = -1017,
  ES_DEVICE_ID_MISMATCH = -1020,
  ES_HASH_MISMATCH = -1022,
  ES_ENOMEM = -1024,
  ES_EACCES = -1026,
  ES_UNKNOWN_ISSUER = -1027,
  ES_NO_TICKET = -1028,
  ES_INVALID_TICKET = -1029,
  // IOSC (crypto engine)
  IOSC_EACCES = -2000,
  IOSC_EEXIST = -2001,
  IOSC_EINVAL = -2002,
  IOSC_EMAX = -2003,
  IOSC_ENOENT = -2004,
  IOSC_INVALID_OBJTYPE = -2005,
  IOSC_INVALID_RNG = -2006,
  IOSC_INVALID_FLAG = -2007,
  IOSC_INVALID_FORMAT = -2008,
  IOSC_INVALID_VERSION = -2009,
  IOSC_INVALID_SIGNER = -2010,
  IOSC_FAIL_CHECKVALUE = -2011,
  IOSC_FAIL_INTERNAL = -2012,
  IOSC_FAIL_ALLOC = -2013,
  IOSC_INVALID_SIZE = -2014,
  IOSC_INVALID_ADDR = -2015,
  IOSC_INVALID_ALIGN = -2016,
  // USB: a pending OH0 device-change hook was cancelled.
  USB_ECANCELED = -7022,
};

// Common part of every IPC request block in PPC memory: command word at +0, file descriptor
// at +8. The argument words that follow from +0xc depend on the command.
struct Request
{
  u32 address = 0;
  IPCCommandType command = IPC_CMD_OPEN;
  u32 fd = 0;
  Request(Core::System& system, u32 address);
  virtual ~Request() = default;
};

enum OpenMode : s32
{
  IOS_OPEN_NONE = 0,
  IOS_OPEN_READ = 1,
  IOS_OPEN_WRITE = 2,
  IOS_OPEN_RW = (IOS_OPEN_READ | IOS_OPEN_WRITE)
};

struct OpenRequest final : Request
{
  std::string path;
  OpenMode flags = IOS_OPEN_READ;
  // The requester's identity. The PPC does not send these; the kernel fills them in from the
  // credentials of the running PPC title before handing the request to a module.
  u32 uid = 0;
  u16 gid = 0;
  OpenRequest(Core::System& system, u32 address);
};

struct ReadWriteRequest final : Request
{
  u32 buffer = 0;
  u32 size = 0;
  ReadWriteRequest(Core::System& system, u32 address);
};

enum SeekMode : s32
{
  IOS_SEEK_SET = 0,
  IOS_SEEK_CUR = 1,
  IOS_SEEK_END = 2,
};

struct SeekRequest final : Request
{
  u32 offset = 0;
  SeekMode mode = IOS_SEEK_SET;
  SeekRequest(Core::System& system, u32 address);
};

struct IOCtlRequest final : Request
{
  u32 request = 0;
  u32 buffer_in = 0;
  u32 buffer_in_size = 0;
  // Some modules also read from this buffer, despite its name.
  u32 buffer_out = 0;
  u32 buffer_out_size = 0;
  IOCtlRequest(Core::System& system, u32 address);
  void Log(std::string_view description, Common::Log::LogType type = Common::Log::LogType::IOS,
           Common::Log::LogLevel level = Common::Log::LogLevel::LINFO) const;
  void Dump(Core::System& system, const std::string& description,
            Common::Log::LogType type = Common::Log::LogType::IOS,
            Common::Log::LogLevel level = Common::Log::LogLevel::LINFO) const;
  void DumpUnknown(Core::System& system, const std::string& description,
                   Common::Log::LogType type = Common::Log::LogType::IOS,
                   Common::Log::LogLevel level = Common::Log::LogLevel::LERROR) const;
};

struct IOCtlVRequest final : Request
{
  struct IOVector
  {
    u32 address = 0;
    u32 size = 0;
  };
  u32 request = 0;
  // The vector table holds the "in" vectors first and the "io" vectors after them. Either kind
  // may be read or written by a module in practice (the network modules write to in vectors),
  // but they are kept in two lists so that io_vectors[0] is easy to reach.
  std::vector<IOVector> in_vectors;
  std::vector<IOVector> io_vectors;

  // Indexes the combined in+io list; nullptr past the end.
  const IOVector* GetVector(size_t index) const;

  IOCtlVRequest(Core::System& system, u32 address);
  bool HasNumberOfValidVectors(size_t in_count, size_t io_count) const;
  void Dump(Core::System& system, std::string_view description,
            Common::Log::LogType type = Common::Log::LogType::IOS,
            Common::Log::LogLevel level = Common::Log::LogLevel::LINFO) const;
  void DumpUnknown(Core::System& system, const std::string& description,
                   Common::Log::LogType type = Common::Log::LogType::IOS,
                   Common::Log::LogLevel level = Common::Log::LogLevel::LERROR) const;
};

// Base class of every resource manager the kernel can route requests to.
class Device
{
public:
  enum class DeviceType : u32
  {
    // Registered at kernel start-up under a fixed name.
    Static,
    // A USB device node made on demand under /dev/usb/oh0.
    OH0,
  };

  Device(Kernel& ios, std::string device_name, DeviceType type = DeviceType::Static);

  virtual ~Device() = default;
  virtual void DoState(PointerWrap& p);

  const std::string& GetDeviceName() const { return m_name; }
  // Open and Close results are turned into replies by the kernel's dispatcher.
  virtual std::optional<IPCReply> Open(const OpenRequest& request);
  virtual std::optional<IPCReply> Close(u32 fd);
  virtual std::optional<IPCReply> Seek(const SeekRequest& seek) { return Unsupported(seek); }
  virtual std::optional<IPCReply> Read(const ReadWriteRequest& read) { return Unsupported(read); }
  virtual std::optional<IPCReply> Write(const ReadWriteRequest& write)
  {
    return Unsupported(write);
  }
  virtual std::optional<IPCReply> IOCtl(const IOCtlRequest& ioctl) { return Unsupported(ioctl); }
  virtual std::optional<IPCReply> IOCtlV(const IOCtlVRequest& ioctlv)
  {
    return Unsupported(ioctlv);
  }
  virtual void Update() {}
  virtual void UpdateWantDeterminism(bool /*new_want_determinism*/) {}
  virtual DeviceType GetDeviceType() const { return m_device_type; }
  virtual bool IsOpened() const { return m_is_active; }

protected:
  Kernel& m_ios;

  std::string m_name;
  // Saved in DoState.
  DeviceType m_device_type;
  bool m_is_active = false;

private:
  std::optional<IPCReply> Unsupported(const Request& request);
};

// A Device that is only ever created by an EmulationKernel, so it can reach the emulated
// system through a static downcast instead of a runtime check.
class EmulationDevice : public Device
{
public:
  EmulationDevice(EmulationKernel& ios, const std::string& device_name,
                  DeviceType type = DeviceType::Static)
      : Device(ios, device_name, type)
  {
  }

  EmulationKernel& GetEmulationKernel() const { return static_cast<EmulationKernel&>(m_ios); }

  Core::System& GetSystem() const { return GetEmulationKernel().GetSystem(); }
};
}  // namespace IOS::HLE
