// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/IOSC.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace Memory
{
class MemoryManager;
}

namespace IOS::HLE
{
namespace FS
{
class FileSystem;
}

class Device;
class ESCore;
class ESDevice;
class FSCore;
class FSDevice;
class WiiSockMan;

struct Request;
struct OpenRequest;

// What a device hands back to the kernel for one request: the value for the reply word and
// how long, in timebase ticks, the reply should take to arrive.
struct IPCReply
{
  // Uses a typical reply latency. Prefer the two-argument form when the real cost is known.
  explicit IPCReply(s32 return_value_);
  explicit IPCReply(s32 return_value_, u64 reply_delay_ticks_);

  s32 return_value;
  u64 reply_delay_ticks;
};

// Smallest time a resource manager has been measured to take to answer a request.
constexpr SystemTimers::TimeBaseTick IPC_OVERHEAD_TICKS = 2700_tbticks;

// An optional accumulator for emulated time. Functions that do timed work take one and add
// their cost to it; callers that do not care pass nothing.
class Ticks
{
public:
  Ticks(u64* ticks = nullptr) : m_ticks(ticks) {}

  void Add(u64 ticks)
  {
    if (m_ticks != nullptr)
      *m_ticks += ticks;
  }

private:
  u64* m_ticks = nullptr;
};

// Runs fn with a Ticks that starts at `ticks` and returns its result together with the total.
template <typename ResultProducer>
IPCReply MakeIPCReply(u64 ticks, const ResultProducer& fn)
{
  const s32 result_value = fn(Ticks{&ticks});
  return IPCReply{result_value, ticks};
}

template <typename ResultProducer>
IPCReply MakeIPCReply(const ResultProducer& fn)
{
  return MakeIPCReply(0, fn);
}

enum IPCCommandType : u32
{
  IPC_CMD_OPEN = 1,
  IPC_CMD_CLOSE = 2,
  IPC_CMD_READ = 3,
  IPC_CMD_WRITE = 4,
  IPC_CMD_SEEK = 5,
  IPC_CMD_IOCTL = 6,
  IPC_CMD_IOCTLV = 7,
  // Written over the command word once a request has been answered.
  IPC_REPLY = 8,
};

enum class MemorySetupType
{
  IOSReload,
  Full,
};

enum class HangPPC : bool
{
  No = false,
  Yes = true,
};

void RAMOverrideForIOSMemoryValues(Memory::MemoryManager& memory, MemorySetupType setup_type);

void WriteReturnValue(Memory::MemoryManager& memory, s32 value, u32 address);

// The IOS kernel on its own: the file system and ES cores plus the crypto engine. This is
// enough for tools that work on the NAND without running a game.
class Kernel
{
public:
  explicit Kernel(IOSC::ConsoleType console_type = IOSC::ConsoleType::Retail);
  virtual ~Kernel();

  // FS and ES live in the kernel image itself, so they exist before any module is loaded.
  std::shared_ptr<FS::FileSystem> GetFS();
  FSCore& GetFSCore();
  ESCore& GetESCore();

  u32 GetVersion() const;

  IOSC& GetIOSC();

protected:
  explicit Kernel(u64 title_id);

  std::unique_ptr<FSCore> m_fs_core;
  std::unique_ptr<ESCore> m_es_core;

  bool m_is_responsible_for_nand_root = false;
  u64 m_title_id = 0;

  IOSC m_iosc;
  std::shared_ptr<FS::FileSystem> m_fs;
  std::shared_ptr<WiiSockMan> m_socket_manager;
};

// The kernel that runs alongside emulation: it owns the IPC queues, the fd table and every
// resource manager the current IOS version provides.
class EmulationKernel final : public Kernel
{
public:
  EmulationKernel(Core::System& system, u64 ios_title_id);
  ~EmulationKernel() override;

  // Looks up a registered (static) resource manager by its path.
  std::shared_ptr<Device> GetDeviceByName(std::string_view device_name);
  std::shared_ptr<Device> GetDeviceByFileDescriptor(const u32 fd);

  std::shared_ptr<FSDevice> GetFSDevice();
  std::shared_ptr<ESDevice> GetESDevice();

  void DoState(PointerWrap& p);
  void UpdateDevices();
  void UpdateWantDeterminism(bool new_want_determinism);

  std::shared_ptr<WiiSockMan> GetSocketManager();

  void HandleIPCEvent(u64 userdata);
  void UpdateIPC();

  void EnqueueIPCRequest(u32 address);
  void EnqueueIPCReply(const Request& request, s32 return_value, s64 cycles_in_future = 0,
                       CoreTiming::FromThread from = CoreTiming::FromThread::CPU);

  void SetUidForPPC(u32 uid);
  u32 GetUidForPPC() const;
  void SetGidForPPC(u16 gid);
  u16 GetGidForPPC() const;

  bool BootstrapPPC(const std::string& boot_content_path);
  bool BootIOS(u64 ios_title_id, HangPPC hang_ppc = HangPPC::No,
               const std::string& boot_content_path = {});
  void InitIPC();

  Core::System& GetSystem() const { return m_system; }

private:
  void ExecuteIPCCommand(u32 address);
  std::optional<IPCReply> HandleIPCCommand(const Request& request);

  void AddDevice(std::unique_ptr<Device> device);

  void AddStaticDevices();
  s32 GetFreeDeviceID();
  std::optional<IPCReply> OpenDevice(OpenRequest& request);

  Core::System& m_system;

  static constexpr u8 IPC_MAX_FDS = 0x18;
  std::map<std::string, std::shared_ptr<Device>, std::less<>> m_device_map;
  // A single table shared by everything on the PPC side (real IOS keeps one per process).
  std::array<std::shared_ptr<Device>, IPC_MAX_FDS> m_fdmap;

  u32 m_ppc_uid = 0;
  u16 m_ppc_gid = 0;

  using IPCMsgQueue = std::deque<u32>;
  // Requests waiting to be acknowledged, and answered requests waiting to be delivered.
  IPCMsgQueue m_request_queue;
  IPCMsgQueue m_reply_queue;
  u64 m_last_reply_time = 0;
  bool m_ipc_paused = false;
};

// Create and destroy the kernel that belongs to an emulated system.
void Init(Core::System& system);
void Shutdown(Core::System& system);

}  // namespace IOS::HLE
