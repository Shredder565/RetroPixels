// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/OH0/OH0.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/USBV0.h"
#include "Core/IOS/VersionInfo.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Root hub descriptor A as read from a console; IOS answers with this fixed value.
constexpr u32 ROOT_HUB_DESCRIPTOR_A = 0x02000302;
// Each device list entry: a zero word, then the vendor and product IDs.
constexpr u32 DEVICE_ENTRY_SIZE = 8;
}  // namespace

OH0::OH0(EmulationKernel& ios, const std::string& device_name) : USBHost(ios, device_name)
{
}

OH0::~OH0() = default;

std::optional<IPCReply> OH0::Open(const OpenRequest& request)
{
  // IOS versions with the new USB stack have no oh0.
  if (HasFeature(m_ios.GetVersion(), Feature::NewUSB))
    return IPCReply(IPC_EACCES);
  return USBHost::Open(request);
}

std::optional<IPCReply> OH0::IOCtl(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_USB);
  if (request.request == USB::IOCTL_USBV0_GETRHDESCA)
    return RootHubDescriptorA(request);
  if (request.request == USB::IOCTL_USBV0_CANCEL_INSERT_HOOK)
    return CancelInsertHook(request);
  return IPCReply(IPC_EINVAL);
}

std::optional<IPCReply> OH0::IOCtlV(const IOCtlVRequest& request)
{
  INFO_LOG_FMT(IOS_USB, "{}: ioctlv {}", GetDeviceName(), request.request);
  switch (request.request)
  {
  case USB::IOCTLV_USBV0_GETDEVLIST:
    return ListDevices(request);
  case USB::IOCTLV_USBV0_GETRHPORTSTATUS:
    return RootHubPortStatus(request, false);
  case USB::IOCTLV_USBV0_SETRHPORTSTATUS:
    return RootHubPortStatus(request, true);
  case USB::IOCTLV_USBV0_DEVINSERTHOOK:
    return AddInsertHook(request);
  case USB::IOCTLV_USBV0_DEVICECLASSCHANGE:
    return AddClassChangeHook(request);
  case USB::IOCTLV_USBV0_DEVINSERTHOOKID:
    return AddInsertHookWithId(request);
  default:
    return IPCReply(IPC_EINVAL);
  }
}

void OH0::DoState(PointerWrap& p)
{
  {
    std::lock_guard lock(m_devices_mutex);
    const bool loading_with_devices = p.IsReadMode() && !m_devices.empty();
    if (loading_with_devices)
    {
      // Host devices cannot be put back into the state they had when the save was made.
      Core::DisplayMessage("It is suggested that you unplug and replug all connected USB devices.",
                           5000);
      Core::DisplayMessage("If USB doesn't work properly, an emulation reset may be needed.", 5000);
    }
    p.Do(m_insert_hooks);
    p.Do(m_removal_hooks);
    p.Do(m_open_devices);
  }
  USBHost::DoState(p);
}

IPCReply OH0::RootHubDescriptorA(const IOCtlRequest& request) const
{
  if (request.buffer_out == 0 || request.buffer_out_size != 4)
    return IPCReply(IPC_EINVAL);

  GetSystem().GetMemory().Write_U32(ROOT_HUB_DESCRIPTOR_A, request.buffer_out);
  request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB,
               Common::Log::LogLevel::LWARNING);
  return IPCReply(IPC_SUCCESS);
}

IPCReply OH0::CancelInsertHook(const IOCtlRequest& request)
{
  if (request.buffer_in == 0 || request.buffer_in_size != 4)
    return IPCReply(IPC_EINVAL);

  // The hook ID is the one AddInsertHookWithId handed out: vendor ID, then product ID.
  const Memory::MemoryManager& memory = GetSystem().GetMemory();
  const USB::DeviceInfo key{memory.Read_U16(request.buffer_in),
                            memory.Read_U16(request.buffer_in + 2)};
  FireHook(m_insert_hooks, key, USB_ECANCELED);
  return IPCReply(IPC_SUCCESS);
}

IPCReply OH0::ListDevices(const IOCtlVRequest& request) const
{
  // in: maximum entry count (u8), interface class (u8); io: entry count (u8), entries.
  if (!request.HasNumberOfValidVectors(2, 2))
    return IPCReply(IPC_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u32 capacity = memory.Read_U8(request.in_vectors[0].address);
  if (request.io_vectors[1].size != capacity * DEVICE_ENTRY_SIZE)
    return IPCReply(IPC_EINVAL);

  const u8 wanted_class = memory.Read_U8(request.in_vectors[1].address);
  const u32 list = request.io_vectors[1].address;
  u32 written = 0;

  std::lock_guard lock(m_devices_mutex);
  for (const auto& entry : m_devices)
  {
    if (written >= capacity)
      break;
    const USB::Device& device = *entry.second;
    if (!device.HasClass(wanted_class))
      continue;

    const u32 slot = list + written * DEVICE_ENTRY_SIZE;
    memory.Write_U32(0, slot);
    memory.Write_U16(device.GetVid(), slot + 4);
    memory.Write_U16(device.GetPid(), slot + 6);
    ++written;
  }
  memory.Write_U8(static_cast<u8>(written), request.io_vectors[0].address);
  return IPCReply(IPC_SUCCESS);
}

IPCReply OH0::RootHubPortStatus(const IOCtlVRequest& request, bool set) const
{
  // Reading takes one in and one io vector; writing takes two in vectors. Neither is emulated.
  const bool valid = set ? request.HasNumberOfValidVectors(2, 0) :
                           request.HasNumberOfValidVectors(1, 1);
  if (!valid)
    return IPCReply(IPC_EINVAL);

  ERROR_LOG_FMT(IOS_USB, "{}: root hub port status {} is not implemented", GetDeviceName(),
                set ? "write" : "read");
  request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB,
               Common::Log::LogLevel::LERROR);
  return IPCReply(IPC_SUCCESS);
}

std::optional<IPCReply> OH0::AddInsertHook(const IOCtlVRequest& request)
{
  if (!request.HasNumberOfValidVectors(2, 0))
    return IPCReply(IPC_EINVAL);

  const Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u16 vid = memory.Read_U16(request.in_vectors[0].address);
  const u16 pid = memory.Read_U16(request.in_vectors[1].address);
  // A device that is already there satisfies the hook straight away.
  if (IsPresent(vid, pid))
    return IPCReply(IPC_SUCCESS);

  std::lock_guard lock(m_hook_mutex);
  // A newer hook replaces an older one with the same key.
  m_insert_hooks[USB::DeviceInfo{vid, pid}] = request.address;
  return std::nullopt;
}

std::optional<IPCReply> OH0::AddInsertHookWithId(const IOCtlVRequest& request)
{
  // in: vendor ID, product ID, "new devices only" flag; io: the hook ID (for cancelling).
  if (!request.HasNumberOfValidVectors(3, 1))
    return IPCReply(IPC_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u16 vid = memory.Read_U16(request.in_vectors[0].address);
  const u16 pid = memory.Read_U16(request.in_vectors[1].address);
  const bool new_devices_only = memory.Read_U8(request.in_vectors[2].address) == 1;
  if (!new_devices_only && IsPresent(vid, pid))
    return IPCReply(IPC_SUCCESS);

  std::lock_guard lock(m_hook_mutex);
  // Unlike the plain hook, an existing hook with the same key is kept.
  m_insert_hooks.emplace(USB::DeviceInfo{vid, pid}, request.address);
  // Real IOS makes up an ID; using the IDs themselves lets CancelInsertHook find the hook.
  memory.Write_U32((u32{vid} << 16) | pid, request.io_vectors[0].address);
  return std::nullopt;
}

std::optional<IPCReply> OH0::AddClassChangeHook(const IOCtlVRequest& request) const
{
  if (!request.HasNumberOfValidVectors(1, 0))
    return IPCReply(IPC_EINVAL);

  // Never answered: class changes are not emulated.
  WARN_LOG_FMT(IOS_USB, "{}: device class change hooks are not implemented; not replying",
               GetDeviceName());
  request.Dump(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB,
               Common::Log::LogLevel::LWARNING);
  return std::nullopt;
}

std::optional<IPCReply> OH0::AddRemovalHook(const u64 device_id, const IOCtlRequest& request)
{
  std::lock_guard lock(m_hook_mutex);
  // One removal hook per device.
  if (!m_removal_hooks.emplace(device_id, request.address).second)
    return IPCReply(IPC_EEXIST);
  return std::nullopt;
}

bool OH0::IsPresent(const u16 vid, const u16 pid) const
{
  std::lock_guard lock(m_devices_mutex);
  for (const auto& entry : m_devices)
  {
    if (entry.second->GetVid() == vid && entry.second->GetPid() == pid)
      return true;
  }
  return false;
}

void OH0::OnDeviceChange(const ChangeEvent event, std::shared_ptr<USB::Device> device)
{
  switch (event)
  {
  case ChangeEvent::Inserted:
    FireHook(m_insert_hooks, USB::DeviceInfo{device->GetVid(), device->GetPid()}, IPC_SUCCESS);
    break;
  case ChangeEvent::Removed:
  {
    FireHook(m_removal_hooks, device->GetId(), IPC_SUCCESS);
    // Forget that it was open, so that it can be opened again once it comes back (Rock Band 3
    // microphones otherwise go missing after a reconnect).
    std::lock_guard lock(m_devices_mutex);
    m_open_devices.erase(device->GetId());
    break;
  }
  }
}

template <typename Key>
void OH0::FireHook(std::map<Key, u32>& hooks, const Key& key, const ReturnCode result)
{
  std::lock_guard lock(m_hook_mutex);
  const auto hook = hooks.find(key);
  if (hook == hooks.end())
    return;

  const u32 address = hook->second;
  hooks.erase(hook);
  GetEmulationKernel().EnqueueIPCReply(Request{GetSystem(), address}, result, 0,
                                       CoreTiming::FromThread::ANY);
}

std::pair<ReturnCode, u64> OH0::DeviceOpen(const u16 vid, const u16 pid)
{
  std::lock_guard lock(m_devices_mutex);

  bool found_any = false;
  for (const auto& entry : m_devices)
  {
    USB::Device& device = *entry.second;
    if (device.GetVid() != vid || device.GetPid() != pid)
      continue;
    found_any = true;

    // A device can only be open once; try the next one with the same IDs.
    const u64 id = device.GetId();
    if (m_open_devices.contains(id) || !device.Attach())
      continue;

    m_open_devices.insert(id);
    return {IPC_SUCCESS, id};
  }
  return {found_any ? IPC_EEXIST : IPC_ENOENT, 0};
}

void OH0::DeviceClose(const u64 device_id)
{
  // A pending removal hook is answered with ENOENT when its device is closed.
  FireHook(m_removal_hooks, device_id, IPC_ENOENT);
  std::lock_guard lock(m_devices_mutex);
  m_open_devices.erase(device_id);
}

std::optional<IPCReply> OH0::DeviceIOCtl(const u64 device_id, const IOCtlRequest& request)
{
  if (!GetDeviceById(device_id))
    return IPCReply(IPC_ENOENT);

  switch (request.request)
  {
  case USB::IOCTL_USBV0_DEVREMOVALHOOK:
    return AddRemovalHook(device_id, request);
  case USB::IOCTL_USBV0_SUSPENDDEV:
  case USB::IOCTL_USBV0_RESUMEDEV:
    // There is no host power management to forward these to.
    return IPCReply(IPC_SUCCESS);
  case USB::IOCTL_USBV0_RESET_DEVICE:
    // A reset looks like a removal to whoever is waiting for one.
    FireHook(m_removal_hooks, device_id, IPC_SUCCESS);
    return IPCReply(IPC_SUCCESS);
  default:
    return IPCReply(IPC_EINVAL);
  }
}

std::optional<IPCReply> OH0::DeviceIOCtlV(const u64 device_id, const IOCtlVRequest& request)
{
  const std::shared_ptr<USB::Device> device = GetDeviceById(device_id);
  if (!device)
    return IPCReply(IPC_ENOENT);

  constexpr std::array<u32, 5> TRANSFERS = {
      USB::IOCTLV_USBV0_CTRLMSG, USB::IOCTLV_USBV0_BLKMSG, USB::IOCTLV_USBV0_LBLKMSG,
      USB::IOCTLV_USBV0_INTRMSG, USB::IOCTLV_USBV0_ISOMSG};
  if (std::ranges::find(TRANSFERS, request.request) != TRANSFERS.end())
  {
    return HandleTransfer(device, request.request,
                          [&] { return StartTransfer(*device, request); });
  }

  // Command 32 is unknown; it is logged and accepted.
  if (request.request != USB::IOCTLV_USBV0_UNKNOWN_32)
    return IPCReply(IPC_EINVAL);
  request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB);
  return IPCReply(IPC_SUCCESS);
}

s32 OH0::StartTransfer(USB::Device& device, const IOCtlVRequest& ioctlv)
{
  const Memory::MemoryManager& memory = GetSystem().GetMemory();
  EmulationKernel& ios = GetEmulationKernel();

  // The length the guest states must match the size of the data vector.
  const auto length_matches = [&](size_t in_vector, bool little_endian) {
    u16 length = memory.Read_U16(ioctlv.in_vectors[in_vector].address);
    if (little_endian)
      length = Common::swap16(length);
    return length == ioctlv.io_vectors[0].size;
  };

  switch (ioctlv.request)
  {
  case USB::IOCTLV_USBV0_CTRLMSG:
    // The setup packet's wLength (vector 4) is little endian.
    if (!ioctlv.HasNumberOfValidVectors(6, 1) || !length_matches(4, true))
      return IPC_EINVAL;
    return device.SubmitTransfer(std::make_unique<USB::V0CtrlMessage>(ios, ioctlv));

  case USB::IOCTLV_USBV0_BLKMSG:
  case USB::IOCTLV_USBV0_LBLKMSG:
  {
    if (!ioctlv.HasNumberOfValidVectors(2, 1) || !length_matches(1, false))
      return IPC_EINVAL;
    const bool long_length = ioctlv.request == USB::IOCTLV_USBV0_LBLKMSG;
    return device.SubmitTransfer(std::make_unique<USB::V0BulkMessage>(ios, ioctlv, long_length));
  }

  case USB::IOCTLV_USBV0_INTRMSG:
    if (!ioctlv.HasNumberOfValidVectors(2, 1) || !length_matches(1, false))
      return IPC_EINVAL;
    return device.SubmitTransfer(std::make_unique<USB::V0IntrMessage>(ios, ioctlv));

  case USB::IOCTLV_USBV0_ISOMSG:
    if (!ioctlv.HasNumberOfValidVectors(3, 2))
      return IPC_EINVAL;
    return device.SubmitTransfer(std::make_unique<USB::V0IsoMessage>(ios, ioctlv));

  default:
    return IPC_EINVAL;
  }
}
}  // namespace IOS::HLE
