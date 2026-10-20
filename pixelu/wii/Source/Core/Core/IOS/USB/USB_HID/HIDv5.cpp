// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/USB_HID/HIDv5.h"

#include <algorithm>
#include <memory>
#include <mutex>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/USB/Common.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
constexpr u8 USB_CLASS_HID = 0x03;

// GETDEVPARAMS reply (0x60 bytes): device ID, a constant 1, then the device, configuration and
// interface descriptors and the interrupt IN and OUT endpoint descriptors.
constexpr u32 PARAMS_SIZE = 0x60;
constexpr u32 PARAMS_DEVICE = 36;
constexpr u32 PARAMS_CONFIG = 56;
constexpr u32 PARAMS_INTERFACE = 68;
constexpr u32 PARAMS_ENDPOINT_IN = 80;
constexpr u32 PARAMS_ENDPOINT_OUT = 88;

constexpr u8 ENDPOINT_TYPE_INTERRUPT = 0x03;
constexpr u8 ENDPOINT_DIRECTION_IN = 0x80;

template <typename Descriptor>
void WriteSwapped(Memory::MemoryManager& memory, u32 address, Descriptor descriptor)
{
  descriptor.Swap();
  memory.CopyToEmu(address, &descriptor, sizeof(descriptor));
}
}  // namespace

USB_HIDv5::~USB_HIDv5() = default;

USB_HIDv5::InterruptEndpoints& USB_HIDv5::EndpointsOf(const USBV5Device& device)
{
  return m_interrupt_endpoints[static_cast<size_t>(&device - m_usbv5_devices.data())];
}

std::optional<IPCReply> USB_HIDv5::IOCtl(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_USB);

  const auto on_device = [&](IPCReply (USB_HIDv5::*handler)(USBV5Device&, const IOCtlRequest&)) {
    return HandleDeviceIOCtl(request,
                             [&](USBV5Device& device) { return (this->*handler)(device, request); });
  };

  switch (request.request)
  {
  case USB::IOCTL_USBV5_GETVERSION:
    return GetUSBVersion(request);
  case USB::IOCTL_USBV5_GETDEVICECHANGE:
    return GetDeviceChange(request);
  case USB::IOCTL_USBV5_SHUTDOWN:
    return Shutdown(request);
  case USB::IOCTL_USBV5_GETDEVPARAMS:
    return on_device(&USB_HIDv5::GetDeviceInfo);
  case USB::IOCTL_USBV5_ATTACHFINISH:
    return IPCReply(IPC_SUCCESS);
  case USB::IOCTL_USBV5_SUSPEND_RESUME:
    return on_device(&USB_HIDv5::SuspendResume);
  case USB::IOCTL_USBV5_CANCELENDPOINT:
    return on_device(&USB_HIDv5::CancelEndpoint);
  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB,
                        Common::Log::LogLevel::LERROR);
    return IPCReply(IPC_SUCCESS);
  }
}

std::optional<IPCReply> USB_HIDv5::IOCtlV(const IOCtlVRequest& request)
{
  const bool is_transfer =
      request.request == USB::IOCTLV_USBV5_CTRLMSG || request.request == USB::IOCTLV_USBV5_INTRMSG;
  if (!is_transfer)
  {
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB);
    return IPCReply(IPC_EINVAL);
  }

  // IOS does not check the vector count; it is checked here to stay inside the vectors.
  // (Unlike VEN, HIDv5 might queue transfers by length; that is not emulated.)
  if (request.in_vectors.size() + request.io_vectors.size() != 2)
    return IPCReply(IPC_EINVAL);

  std::lock_guard lock(m_usbv5_devices_mutex);
  USBV5Device* const device = GetUSBV5Device(request.in_vectors[0].address);
  if (device == nullptr)
    return IPCReply(IPC_EINVAL);

  const std::shared_ptr<USB::Device> host_device = GetDeviceById(device->host_id);
  if (request.request == USB::IOCTLV_USBV5_CTRLMSG)
    host_device->Attach();
  else
    host_device->AttachAndChangeInterface(device->interface_number);

  return HandleTransfer(host_device, request.request,
                        [&] { return SubmitTransfer(*device, *host_device, request); });
}

s32 USB_HIDv5::SubmitTransfer(USBV5Device& device, USB::Device& host_device,
                              const IOCtlVRequest& ioctlv)
{
  if (ioctlv.request == USB::IOCTLV_USBV5_CTRLMSG)
  {
    return host_device.SubmitTransfer(
        std::make_unique<USB::V5CtrlMessage>(GetEmulationKernel(), ioctlv));
  }
  if (ioctlv.request != USB::IOCTLV_USBV5_INTRMSG)
    return IPC_EINVAL;

  // HID picks the endpoint itself: a non-zero word at offset 8 of the request block means
  // "write" (interrupt OUT), zero means "read" (interrupt IN). VEN takes it from the block.
  auto message = std::make_unique<USB::V5IntrMessage>(GetEmulationKernel(), ioctlv);
  const bool is_write = GetSystem().GetMemory().Read_U32(ioctlv.in_vectors[0].address + 8) != 0;
  const InterruptEndpoints& endpoints = EndpointsOf(device);
  message->endpoint = is_write ? endpoints.out : endpoints.in;
  return host_device.SubmitTransfer(std::move(message));
}

IPCReply USB_HIDv5::CancelEndpoint(USBV5Device& device, const IOCtlRequest& request)
{
  // 0 = control, 1 = interrupt IN, 2 = interrupt OUT. IOS also fails the cancelled transfers
  // with -7022 itself; here the backend answers them.
  const u8 which = GetSystem().GetMemory().Read_U8(request.buffer_in + 8);
  u8 endpoint = 0;
  if (which == 1)
    endpoint = EndpointsOf(device).in;
  else if (which == 2)
    endpoint = EndpointsOf(device).out;

  GetDeviceById(device.host_id)->CancelTransfer(endpoint);
  return IPCReply(IPC_SUCCESS);
}

IPCReply USB_HIDv5::GetDeviceInfo(USBV5Device& device, const IOCtlRequest& request)
{
  if (request.buffer_out == 0 || request.buffer_out_size != PARAMS_SIZE)
    return IPCReply(IPC_EINVAL);

  Memory::MemoryManager& memory = GetSystem().GetMemory();
  const std::shared_ptr<USB::Device> host_device = GetDeviceById(device.host_id);
  const u32 out = request.buffer_out;
  const u8 alt_setting = memory.Read_U8(request.buffer_in + 8);

  memory.Memset(out, 0, request.buffer_out_size);
  memory.Write_U32(memory.Read_U32(request.buffer_in), out);
  memory.Write_U32(1, out + 4);
  WriteSwapped(memory, out + PARAMS_DEVICE, host_device->GetDeviceDescriptor());
  // Only the first configuration is ever used.
  WriteSwapped(memory, out + PARAMS_CONFIG, host_device->GetConfigurations()[0]);

  const std::vector<USB::InterfaceDescriptor> interfaces = host_device->GetInterfaces(0);
  const auto match = std::ranges::find_if(interfaces, [&](const USB::InterfaceDescriptor& d) {
    return d.bInterfaceNumber == device.interface_number && d.bAlternateSetting == alt_setting;
  });
  if (match == interfaces.end())
    return IPCReply(IPC_EINVAL);
  WriteSwapped(memory, out + PARAMS_INTERFACE, *match);

  InterruptEndpoints& known = EndpointsOf(device);
  for (const USB::EndpointDescriptor& endpoint :
       host_device->GetEndpoints(0, match->bInterfaceNumber, match->bAlternateSetting))
  {
    if (endpoint.bmAttributes != ENDPOINT_TYPE_INTERRUPT)
      continue;

    const bool is_in = (endpoint.bEndpointAddress & ENDPOINT_DIRECTION_IN) != 0;
    (is_in ? known.in : known.out) = endpoint.bEndpointAddress;
    WriteSwapped(memory, out + (is_in ? PARAMS_ENDPOINT_IN : PARAMS_ENDPOINT_OUT), endpoint);
  }

  return IPCReply(IPC_SUCCESS);
}

bool USB_HIDv5::ShouldAddDevice(const USB::Device& device) const
{
  // HIDv5 opens /dev/usb/usb with mode 3, which presumably selects the HID class (VEN uses 0xff).
  return device.HasClass(USB_CLASS_HID);
}
}  // namespace IOS::HLE
