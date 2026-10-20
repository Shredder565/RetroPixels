// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/USB_VEN/VEN.h"

#include <algorithm>
#include <cstddef>
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
// GETDEVPARAMS reply (0xc0 bytes): device ID, a constant 1, the device, configuration and
// interface descriptors, then every endpoint descriptor of the interface, 8 bytes apart.
constexpr u32 PARAMS_SIZE = 0xc0;
constexpr u32 PARAMS_DEVICE = 20;
constexpr u32 PARAMS_CONFIG = 40;
constexpr u32 PARAMS_INTERFACE = 52;
constexpr u32 PARAMS_ENDPOINTS = 64;
constexpr u32 PARAMS_ENDPOINT_STRIDE = 8;

// Number of vectors each transfer ioctlv must have (in + io).
constexpr size_t VectorsFor(u32 request)
{
  return request == USB::IOCTLV_USBV5_ISOMSG ? 4 : 2;
}

template <typename Descriptor>
void WriteSwapped(Memory::MemoryManager& memory, u32 address, Descriptor descriptor)
{
  descriptor.Swap();
  memory.CopyToEmu(address, &descriptor, sizeof(descriptor));
}
}  // namespace

USB_VEN::~USB_VEN() = default;

std::optional<IPCReply> USB_VEN::IOCtl(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_USB);

  const auto on_device = [&](IPCReply (USB_VEN::*handler)(USBV5Device&, const IOCtlRequest&)) {
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
    return on_device(&USB_VEN::GetDeviceInfo);
  case USB::IOCTL_USBV5_ATTACHFINISH:
    return IPCReply(IPC_SUCCESS);
  case USB::IOCTL_USBV5_SETALTERNATE:
    return on_device(&USB_VEN::SetAlternateSetting);
  case USB::IOCTL_USBV5_SUSPEND_RESUME:
    return on_device(&USB_VEN::SuspendResume);
  case USB::IOCTL_USBV5_CANCELENDPOINT:
    return on_device(&USB_VEN::CancelEndpoint);
  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_USB,
                        Common::Log::LogLevel::LERROR);
    return IPCReply(IPC_SUCCESS);
  }
}

std::optional<IPCReply> USB_VEN::IOCtlV(const IOCtlVRequest& request)
{
  switch (request.request)
  {
  case USB::IOCTLV_USBV5_CTRLMSG:
  case USB::IOCTLV_USBV5_INTRMSG:
  case USB::IOCTLV_USBV5_BULKMSG:
  case USB::IOCTLV_USBV5_ISOMSG:
    break;
  default:
    return IPCReply(IPC_EINVAL);
  }

  if (request.in_vectors.size() + request.io_vectors.size() != VectorsFor(request.request))
    return IPCReply(IPC_EINVAL);

  std::lock_guard lock(m_usbv5_devices_mutex);
  USBV5Device* const device = GetUSBV5Device(request.in_vectors[0].address);
  if (device == nullptr)
    return IPCReply(IPC_EINVAL);

  const std::shared_ptr<USB::Device> host_device = GetDeviceById(device->host_id);
  // Control transfers go to the device as a whole; the others need the right interface.
  if (request.request == USB::IOCTLV_USBV5_CTRLMSG)
    host_device->Attach();
  else
    host_device->AttachAndChangeInterface(device->interface_number);

  return HandleTransfer(host_device, request.request,
                        [&] { return SubmitTransfer(*host_device, request); });
}

s32 USB_VEN::SubmitTransfer(USB::Device& device, const IOCtlVRequest& ioctlv)
{
  EmulationKernel& ios = GetEmulationKernel();
  switch (ioctlv.request)
  {
  case USB::IOCTLV_USBV5_CTRLMSG:
    return device.SubmitTransfer(std::make_unique<USB::V5CtrlMessage>(ios, ioctlv));
  case USB::IOCTLV_USBV5_INTRMSG:
    return device.SubmitTransfer(std::make_unique<USB::V5IntrMessage>(ios, ioctlv));
  case USB::IOCTLV_USBV5_BULKMSG:
    return device.SubmitTransfer(std::make_unique<USB::V5BulkMessage>(ios, ioctlv));
  case USB::IOCTLV_USBV5_ISOMSG:
    return device.SubmitTransfer(std::make_unique<USB::V5IsoMessage>(ios, ioctlv));
  default:
    return IPC_EINVAL;
  }
}

IPCReply USB_VEN::CancelEndpoint(USBV5Device& device, const IOCtlRequest& request)
{
  const u8 endpoint = GetSystem().GetMemory().Read_U8(request.buffer_in + 8);
  // IOS answers -4 when there was nothing to cancel.
  const int result = GetDeviceById(device.host_id)->CancelTransfer(endpoint);
  return IPCReply(result < 0 ? IPC_EINVAL : IPC_SUCCESS);
}

IPCReply USB_VEN::GetDeviceInfo(USBV5Device& device, const IOCtlRequest& request)
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
  // VEN only reports the first configuration.
  WriteSwapped(memory, out + PARAMS_CONFIG, host_device->GetConfigurations()[0]);

  const std::vector<USB::InterfaceDescriptor> interfaces = host_device->GetInterfaces(0);
  const auto match = std::ranges::find_if(interfaces, [&](const USB::InterfaceDescriptor& d) {
    return d.bInterfaceNumber == device.interface_number && d.bAlternateSetting == alt_setting;
  });
  if (match == interfaces.end())
    return IPCReply(IPC_EINVAL);
  WriteSwapped(memory, out + PARAMS_INTERFACE, *match);

  const std::vector<USB::EndpointDescriptor> endpoints =
      host_device->GetEndpoints(0, match->bInterfaceNumber, match->bAlternateSetting);
  for (size_t index = 0; index < endpoints.size(); ++index)
  {
    // The slot number is a byte, as in IOS.
    const u32 slot = static_cast<u8>(index);
    WriteSwapped(memory, out + PARAMS_ENDPOINTS + PARAMS_ENDPOINT_STRIDE * slot, endpoints[index]);
  }

  return IPCReply(IPC_SUCCESS);
}
}  // namespace IOS::HLE
