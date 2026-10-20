// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/USBV5.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <ranges>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Layout of the 32-byte block that starts every USBv5 request. The first word is the device ID:
// a reserved byte, the entry index and the generation number.
constexpr u32 ID_OFFSET_INDEX = 1;
constexpr u32 ID_OFFSET_NUMBER = 2;
// Per-transfer parameters that follow it.
constexpr u32 BLOCK_CTRL_REQUEST_TYPE = 8;
constexpr u32 BLOCK_CTRL_REQUEST = 9;
constexpr u32 BLOCK_CTRL_VALUE = 10;
constexpr u32 BLOCK_CTRL_INDEX = 12;
constexpr u32 BLOCK_INTR_ENDPOINT = 14;
constexpr u32 BLOCK_ISO_PACKET_COUNT = 16;
constexpr u32 BLOCK_ISO_ENDPOINT = 17;
constexpr u32 BLOCK_BULK_ENDPOINT = 18;

// One 12-byte entry of the device-change reply.
constexpr u32 CHANGE_ENTRY_SIZE = 12;

constexpr u32 USBV5_VERSION = 0x50001;

Memory::MemoryManager& GuestMemory(EmulationKernel& ios)
{
  return ios.GetSystem().GetMemory();
}

u32 BlockAddress(const IOCtlVRequest& ioctlv)
{
  return ioctlv.in_vectors[0].address;
}
}  // namespace

namespace USB
{
// Control: io vector 1 holds the data and gives wLength.
V5CtrlMessage::V5CtrlMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv)
    : CtrlMessage(ios, ioctlv, ioctlv.GetVector(1)->address)
{
  const Memory::MemoryManager& memory = GuestMemory(ios);
  const u32 block = BlockAddress(ioctlv);
  request_type = memory.Read_U8(block + BLOCK_CTRL_REQUEST_TYPE);
  request = memory.Read_U8(block + BLOCK_CTRL_REQUEST);
  value = memory.Read_U16(block + BLOCK_CTRL_VALUE);
  index = memory.Read_U16(block + BLOCK_CTRL_INDEX);
  length = static_cast<u16>(ioctlv.GetVector(1)->size);
}

V5BulkMessage::V5BulkMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv)
    : BulkMessage(ios, ioctlv, ioctlv.GetVector(1)->address)
{
  length = ioctlv.GetVector(1)->size;
  endpoint = GuestMemory(ios).Read_U8(BlockAddress(ioctlv) + BLOCK_BULK_ENDPOINT);
}

V5IntrMessage::V5IntrMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv)
    : IntrMessage(ios, ioctlv, ioctlv.GetVector(1)->address)
{
  length = ioctlv.GetVector(1)->size;
  endpoint = GuestMemory(ios).Read_U8(BlockAddress(ioctlv) + BLOCK_INTR_ENDPOINT);
}

// Isochronous: vector 1 is the array of packet lengths, vector 2 the data.
V5IsoMessage::V5IsoMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv)
    : IsoMessage(ios, ioctlv, ioctlv.GetVector(2)->address)
{
  const Memory::MemoryManager& memory = GuestMemory(ios);
  const u32 block = BlockAddress(ioctlv);
  num_packets = memory.Read_U8(block + BLOCK_ISO_PACKET_COUNT);
  endpoint = memory.Read_U8(block + BLOCK_ISO_ENDPOINT);
  packet_sizes_addr = ioctlv.GetVector(1)->address;

  u32 sum_of_packets = 0;
  packet_sizes.reserve(num_packets);
  for (u32 packet = 0; packet < num_packets; ++packet)
  {
    const u16 packet_length = memory.Read_U16(packet_sizes_addr + packet * 2);
    packet_sizes.push_back(packet_length);
    sum_of_packets += packet_length;
  }

  length = ioctlv.GetVector(2)->size;
  ASSERT_MSG(IOS_USB, length == sum_of_packets, "Wrong buffer size ({:#x} != {:#x})", length,
             sum_of_packets);
}
}  // namespace USB

void USBV5ResourceManager::DoState(PointerWrap& p)
{
  p.Do(m_has_pending_changes);

  // Only the guest address of the pending device-change request is saved.
  u32 hook_address = 0;
  if (m_devicechange_hook_request)
    hook_address = m_devicechange_hook_request->address;
  p.Do(hook_address);
  m_devicechange_hook_request =
      hook_address == 0 ? nullptr : std::make_unique<IOCtlRequest>(GetSystem(), hook_address);

  p.Do(m_usbv5_devices);
  USBHost::DoState(p);
}

USBV5ResourceManager::USBV5Device* USBV5ResourceManager::GetUSBV5Device(u32 in_buffer)
{
  const Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u8 slot = memory.Read_U8(in_buffer + ID_OFFSET_INDEX);
  const u16 generation = memory.Read_U16(in_buffer + ID_OFFSET_NUMBER);
  if (slot >= m_usbv5_devices.size())
    return nullptr;

  USBV5Device& entry = m_usbv5_devices[slot];
  const bool matches = entry.in_use && entry.number == generation;
  return matches ? &entry : nullptr;
}

std::optional<IPCReply> USBV5ResourceManager::GetDeviceChange(const IOCtlRequest& request)
{
  std::lock_guard lock(m_devicechange_hook_address_mutex);
  // Only one request may wait at a time, and its buffer must fit 32 entries.
  if (request.buffer_out_size != 32 * CHANGE_ENTRY_SIZE || m_devicechange_hook_request)
    return IPCReply(IPC_EINVAL);

  m_devicechange_hook_request = std::make_unique<IOCtlRequest>(GetSystem(), request.address);
  // News that arrived while nobody was waiting is delivered right away.
  if (m_has_pending_changes)
  {
    TriggerDeviceChangeReply();
    m_has_pending_changes = false;
  }
  return std::nullopt;
}

IPCReply USBV5ResourceManager::SetAlternateSetting(USBV5Device& device,
                                                   const IOCtlRequest& request)
{
  const std::shared_ptr<USB::Device> host_device = GetDeviceById(device.host_id);
  if (!host_device->AttachAndChangeInterface(device.interface_number))
    return IPCReply(-1);

  const u8 alt_setting = GetSystem().GetMemory().Read_U8(request.buffer_in + 8);
  if (host_device->SetAltSetting(alt_setting) != 0)
    return IPCReply(IPC_EINVAL);
  return IPCReply(IPC_SUCCESS);
}

IPCReply USBV5ResourceManager::Shutdown(const IOCtlRequest& request)
{
  const bool has_buffers = request.buffer_in != 0 || request.buffer_in_size != 0 ||
                           request.buffer_out != 0 || request.buffer_out_size != 0;
  if (has_buffers)
    return IPCReply(IPC_EINVAL);

  // Releases the waiting device-change request (with no entries).
  std::lock_guard lock(m_devicechange_hook_address_mutex);
  if (m_devicechange_hook_request)
  {
    GetEmulationKernel().EnqueueIPCReply(*m_devicechange_hook_request, IPC_SUCCESS);
    m_devicechange_hook_request.reset();
  }
  return IPCReply(IPC_SUCCESS);
}

IPCReply USBV5ResourceManager::SuspendResume(USBV5Device& device, const IOCtlRequest& request)
{
  const std::shared_ptr<USB::Device> host_device = GetDeviceById(device.host_id);
  const s32 resume = static_cast<s32>(GetSystem().GetMemory().Read_U32(request.buffer_in + 8));

  // Not emulated: libusb offers no portable power management.
  INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Ignoring a {} request", host_device->GetVid(),
               host_device->GetPid(), device.interface_number,
               resume == 0 ? "suspend" : "resume");
  return IPCReply(IPC_SUCCESS);
}

std::optional<IPCReply> USBV5ResourceManager::HandleDeviceIOCtl(const IOCtlRequest& request,
                                                                const Handler& handler)
{
  if (request.buffer_in == 0 || request.buffer_in_size != 0x20)
    return IPCReply(IPC_EINVAL);

  std::lock_guard lock(m_usbv5_devices_mutex);
  USBV5Device* const device = GetUSBV5Device(request.buffer_in);
  if (device == nullptr)
    return IPCReply(IPC_EINVAL);
  return handler(*device);
}

IPCReply USBV5ResourceManager::GetUSBVersion(const IOCtlRequest& request) const
{
  const bool valid = request.buffer_in == 0 && request.buffer_in_size == 0 &&
                     request.buffer_out != 0 && request.buffer_out_size == 0x20;
  if (!valid)
    return IPCReply(IPC_EINVAL);

  GetSystem().GetMemory().Write_U32(USBV5_VERSION, request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

void USBV5ResourceManager::OnDeviceChange(const ChangeEvent event,
                                          std::shared_ptr<USB::Device> device)
{
  std::lock_guard lock(m_usbv5_devices_mutex);
  const u64 host_id = device->GetId();

  if (event == ChangeEvent::Removed)
  {
    for (USBV5Device& entry : m_usbv5_devices)
    {
      if (entry.host_id == host_id)
        entry.in_use = false;
    }
    return;
  }

  if (event != ChangeEvent::Inserted)
    return;

  // One entry per interface (alternate setting 0 only). Free entries are taken from the end of
  // the table, like IOS does.
  for (const USB::InterfaceDescriptor& interface_descriptor : device->GetInterfaces(0))
  {
    if (interface_descriptor.bAlternateSetting != 0)
      continue;

    auto free_slot = std::ranges::find(m_usbv5_devices | std::views::reverse, false,
                                       &USBV5Device::in_use);
    if (free_slot == m_usbv5_devices.rend())
      return;

    free_slot->in_use = true;
    free_slot->interface_number = interface_descriptor.bInterfaceNumber;
    free_slot->number = m_current_device_number;
    free_slot->host_id = host_id;
  }
}

void USBV5ResourceManager::OnDeviceChangeEnd()
{
  std::lock_guard lock(m_devicechange_hook_address_mutex);
  TriggerDeviceChangeReply();
  ++m_current_device_number;
}

void USBV5ResourceManager::TriggerDeviceChangeReply()
{
  if (!m_devicechange_hook_request)
  {
    m_has_pending_changes = true;
    return;
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const u32 reply_buffer = m_devicechange_hook_request->buffer_out;

  std::lock_guard lock(m_usbv5_devices_mutex);
  u8 written = 0;
  // Listed from the last slot to the first.
  for (size_t slot = m_usbv5_devices.size(); slot-- > 0;)
  {
    const USBV5Device& entry = m_usbv5_devices[slot];
    if (!entry.in_use)
      continue;

    const std::shared_ptr<USB::Device> host_device = GetDeviceById(entry.host_id);
    if (!host_device)
      continue;

    // The first ID byte comes from IOS's internal IPC request address for VEN; real hardware
    // most often shows 0xe7 there, so that is what is reported.
    const u8 id_reserved = HasInterfaceNumberInIDs() ? entry.interface_number : u8{0xe7};
    const u32 at = reply_buffer + CHANGE_ENTRY_SIZE * written;
    memory.Write_U8(id_reserved, at + 0);
    memory.Write_U8(static_cast<u8>(slot), at + 1);
    memory.Write_U16(entry.number, at + 2);
    memory.Write_U16(host_device->GetVid(), at + 4);
    memory.Write_U16(host_device->GetPid(), at + 6);
    memory.Write_U16(entry.number, at + 8);
    memory.Write_U8(entry.interface_number, at + 10);
    memory.Write_U8(static_cast<u8>(host_device->GetNumberOfAltSettings(entry.interface_number)),
                    at + 11);
    ++written;
  }

  GetEmulationKernel().EnqueueIPCReply(*m_devicechange_hook_request, written, 0,
                                       CoreTiming::FromThread::ANY);
  m_devicechange_hook_request.reset();
  INFO_LOG_FMT(IOS_USB, "Reported {} USBv5 device(s) (interfaces count separately)", written);
}
}  // namespace IOS::HLE
