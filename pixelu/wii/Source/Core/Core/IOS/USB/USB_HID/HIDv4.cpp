// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/USB_HID/HIDv4.h"

#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Common/Align.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/USBV4.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
constexpr u32 HIDV4_VERSION = 0x40001;
constexpr u8 USB_CLASS_HID = 0x03;

// Size the guest must give for the device-change reply buffer.
constexpr u32 DEVICE_CHANGE_BUFFER_SIZE = 0x600;
// Written after the last device section (and alone on shutdown).
constexpr u32 END_OF_LIST = 0xffffffff;

void AppendBigEndian32(std::vector<u8>* out, u32 value)
{
  const u32 big_endian = Common::swap32(value);
  const auto* bytes = reinterpret_cast<const u8*>(&big_endian);
  out->insert(out->end(), bytes, bytes + sizeof(big_endian));
}

// Appends one descriptor in guest byte order, padded with zeros to a multiple of 4 bytes.
template <typename Descriptor>
void AppendDescriptor(std::vector<u8>* out, const Descriptor& descriptor)
{
  Descriptor swapped = descriptor;
  swapped.Swap();
  const auto* bytes = reinterpret_cast<const u8*>(&swapped);
  out->insert(out->end(), bytes, bytes + sizeof(Descriptor));
  out->resize(out->size() + (Common::AlignUp(sizeof(Descriptor), 4) - sizeof(Descriptor)), 0);
}

// The descriptors of a device: device, then for each configuration the configuration followed
// by its interfaces (last one first), each followed by its endpoints.
std::vector<u8> SerialiseDescriptors(const USB::Device& device)
{
  std::vector<u8> out;
  AppendDescriptor(&out, device.GetDeviceDescriptor());

  const std::vector<USB::ConfigDescriptor> configurations = device.GetConfigurations();
  for (size_t index = 0; index < configurations.size(); ++index)
  {
    AppendDescriptor(&out, configurations[index]);

    const u8 config = static_cast<u8>(index);
    const std::vector<USB::InterfaceDescriptor> interfaces = device.GetInterfaces(config);
    for (auto it = interfaces.rbegin(); it != interfaces.rend(); ++it)
    {
      AppendDescriptor(&out, *it);
      const auto endpoints = device.GetEndpoints(config, it->bInterfaceNumber, it->bAlternateSetting);
      for (const USB::EndpointDescriptor& endpoint : endpoints)
        AppendDescriptor(&out, endpoint);
    }
  }
  return out;
}
}  // namespace

USB_HIDv4::USB_HIDv4(EmulationKernel& ios, const std::string& device_name)
    : USBHost(ios, device_name)
{
}

USB_HIDv4::~USB_HIDv4() = default;

std::optional<IPCReply> USB_HIDv4::IOCtl(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_USB, Common::Log::LogLevel::LDEBUG);
  switch (request.request)
  {
  case USB::IOCTL_USBV4_GETVERSION:
    return IPCReply(HIDV4_VERSION);
  case USB::IOCTL_USBV4_GETDEVICECHANGE:
    return GetDeviceChange(request);
  case USB::IOCTL_USBV4_SHUTDOWN:
    return Shutdown(request);
  case USB::IOCTL_USBV4_SET_SUSPEND:
    // IOS accepts this without doing anything.
    return IPCReply(IPC_SUCCESS);
  case USB::IOCTL_USBV4_CANCELINTERRUPT:
    return CancelInterrupt(request);
  case USB::IOCTL_USBV4_GET_US_STRING:
  case USB::IOCTL_USBV4_CTRLMSG:
  case USB::IOCTL_USBV4_INTRMSG_IN:
  case USB::IOCTL_USBV4_INTRMSG_OUT:
    return StartTransfer(request);
  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB);
    return IPCReply(IPC_SUCCESS);
  }
}

std::optional<IPCReply> USB_HIDv4::StartTransfer(const IOCtlRequest& request)
{
  // Every transfer takes a 32-byte request block with the device ID at offset 16.
  if (request.buffer_in == 0 || request.buffer_in_size != 32)
    return IPCReply(IPC_EINVAL);

  const s32 guest_id = static_cast<s32>(GetSystem().GetMemory().Read_U32(request.buffer_in + 16));
  const std::shared_ptr<USB::Device> device = FindByGuestId(guest_id);
  if (!device)
    return IPCReply(IPC_ENOENT);
  if (!device->Attach())
    return IPCReply(IPC_EINVAL);

  return HandleTransfer(device, request.request, [&]() -> s32 {
    EmulationKernel& ios = GetEmulationKernel();
    switch (request.request)
    {
    case USB::IOCTL_USBV4_CTRLMSG:
      return device->SubmitTransfer(std::make_unique<USB::V4CtrlMessage>(ios, request));
    case USB::IOCTL_USBV4_GET_US_STRING:
      return device->SubmitTransfer(std::make_unique<USB::V4GetUSStringMessage>(ios, request));
    case USB::IOCTL_USBV4_INTRMSG_IN:
    case USB::IOCTL_USBV4_INTRMSG_OUT:
      return device->SubmitTransfer(std::make_unique<USB::V4IntrMessage>(ios, request));
    default:
      return IPC_EINVAL;
    }
  });
}

IPCReply USB_HIDv4::CancelInterrupt(const IOCtlRequest& request)
{
  // Input: device ID (u32), endpoint (u8).
  if (request.buffer_in == 0 || request.buffer_in_size != 8)
    return IPCReply(IPC_EINVAL);

  const Memory::MemoryManager& memory = GetSystem().GetMemory();
  const std::shared_ptr<USB::Device> device =
      FindByGuestId(static_cast<s32>(memory.Read_U32(request.buffer_in)));
  if (!device)
    return IPCReply(IPC_ENOENT);

  device->CancelTransfer(memory.Read_U8(request.buffer_in + 4));
  return IPCReply(IPC_SUCCESS);
}

std::optional<IPCReply> USB_HIDv4::GetDeviceChange(const IOCtlRequest& request)
{
  std::lock_guard lock(m_devicechange_hook_address_mutex);
  if (request.buffer_out == 0 || request.buffer_out_size != DEVICE_CHANGE_BUFFER_SIZE)
    return IPCReply(IPC_EINVAL);

  m_devicechange_hook_request = std::make_unique<IOCtlRequest>(GetSystem(), request.address);
  // Answer at once if something changed (or a shutdown happened) since the last request.
  if (m_has_pending_changes || m_is_shut_down)
  {
    TriggerDeviceChangeReply();
    m_has_pending_changes = false;
    m_is_shut_down = false;
  }
  return std::nullopt;
}

IPCReply USB_HIDv4::Shutdown(const IOCtlRequest&)
{
  std::lock_guard lock(m_devicechange_hook_address_mutex);
  if (m_devicechange_hook_request)
  {
    // The waiting request is released with an empty list and -1.
    GetSystem().GetMemory().Write_U32(END_OF_LIST, m_devicechange_hook_request->buffer_out);
    GetEmulationKernel().EnqueueIPCReply(*m_devicechange_hook_request, -1);
    m_devicechange_hook_request.reset();
    m_is_shut_down = true;
  }
  return IPCReply(IPC_SUCCESS);
}

void USB_HIDv4::DoState(PointerWrap& p)
{
  p.Do(m_has_pending_changes);

  u32 hook_address = 0;
  if (m_devicechange_hook_request)
    hook_address = m_devicechange_hook_request->address;
  p.Do(hook_address);
  m_devicechange_hook_request =
      hook_address == 0 ? nullptr : std::make_unique<IOCtlRequest>(GetSystem(), hook_address);

  p.Do(m_ios_ids);
  p.Do(m_device_ids);

  USBHost::DoState(p);
}

std::shared_ptr<USB::Device> USB_HIDv4::FindByGuestId(const s32 guest_id) const
{
  std::lock_guard lock(m_id_map_mutex);
  const auto found = m_ios_ids.find(guest_id);
  if (found == m_ios_ids.end())
    return nullptr;
  return GetDeviceById(found->second);
}

void USB_HIDv4::OnDeviceChange(ChangeEvent event, std::shared_ptr<USB::Device> device)
{
  {
    std::lock_guard lock(m_id_map_mutex);
    const u64 host_id = device->GetId();
    if (event == ChangeEvent::Inserted)
    {
      // The smallest ID not in use.
      s32 guest_id = 0;
      while (m_ios_ids.contains(guest_id))
        ++guest_id;
      m_ios_ids.emplace(guest_id, host_id);
      m_device_ids[host_id] = guest_id;
    }
    else if (event == ChangeEvent::Removed)
    {
      const auto found = m_device_ids.find(host_id);
      if (found != m_device_ids.end())
      {
        m_ios_ids.erase(found->second);
        m_device_ids.erase(found);
      }
    }
  }

  std::lock_guard lock(m_devicechange_hook_address_mutex);
  TriggerDeviceChangeReply();
}

bool USB_HIDv4::ShouldAddDevice(const USB::Device& device) const
{
  return device.HasClass(USB_CLASS_HID);
}

void USB_HIDv4::TriggerDeviceChangeReply()
{
  if (!m_devicechange_hook_request)
  {
    m_has_pending_changes = true;
    return;
  }

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  {
    std::lock_guard lock(m_devices_mutex);
    const u32 reply_buffer = m_devicechange_hook_request->buffer_out;
    // One byte less than the buffer is usable, so that the end marker always fits.
    const u32 usable = m_devicechange_hook_request->buffer_out_size - 1;
    u32 used = 0;
    for (const auto& entry : m_devices)
    {
      const std::vector<u8> section = BuildDeviceSection(*entry.second);
      if (used + section.size() > usable)
      {
        WARN_LOG_FMT(IOS_USB, "Device list is full; the remaining devices are not reported");
        break;
      }
      memory.CopyToEmu(reply_buffer + used, section.data(), section.size());
      used += Common::AlignUp(static_cast<u32>(section.size()), 4);
    }
    memory.Write_U32(END_OF_LIST, reply_buffer + used);
  }

  GetEmulationKernel().EnqueueIPCReply(*m_devicechange_hook_request, IPC_SUCCESS, 0,
                                       CoreTiming::FromThread::ANY);
  m_devicechange_hook_request.reset();
}

std::vector<u8> USB_HIDv4::BuildDeviceSection(const USB::Device& device) const
{
  std::lock_guard lock(m_id_map_mutex);

  // A section is: total size (including these two words), the guest device ID, then the
  // descriptors.
  const std::vector<u8> descriptors = SerialiseDescriptors(device);
  std::vector<u8> section;
  section.reserve(8 + descriptors.size());
  AppendBigEndian32(&section, static_cast<u32>(8 + descriptors.size()));
  AppendBigEndian32(&section, static_cast<u32>(m_device_ids.at(device.GetId())));
  section.insert(section.end(), descriptors.begin(), descriptors.end());
  return section;
}
}  // namespace IOS::HLE
