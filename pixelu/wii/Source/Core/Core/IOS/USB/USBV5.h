// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/Host.h"

class PointerWrap;

// The USB interface of /dev/usb/ven and the later /dev/usb/hid (version 5), introduced when
// IOS57 reorganised the USB modules. Every request names its device with a 32-byte block whose
// first word is the device ID handed out in the device-change reply.

namespace IOS::HLE
{
struct IOCtlRequest;

namespace USB
{
enum V5Requests
{
  IOCTL_USBV5_GETVERSION = 0,
  IOCTL_USBV5_GETDEVICECHANGE = 1,
  IOCTL_USBV5_SHUTDOWN = 2,
  IOCTL_USBV5_GETDEVPARAMS = 3,
  IOCTL_USBV5_ATTACHFINISH = 6,
  IOCTL_USBV5_SETALTERNATE = 7,
  IOCTL_USBV5_SUSPEND_RESUME = 16,
  IOCTL_USBV5_CANCELENDPOINT = 17,
  IOCTLV_USBV5_CTRLMSG = 18,
  IOCTLV_USBV5_INTRMSG = 19,
  IOCTLV_USBV5_ISOMSG = 20,
  IOCTLV_USBV5_BULKMSG = 21
};

struct V5CtrlMessage final : CtrlMessage
{
  V5CtrlMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv);
};

struct V5BulkMessage final : BulkMessage
{
  V5BulkMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv);
};

struct V5IntrMessage final : IntrMessage
{
  V5IntrMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv);
};

struct V5IsoMessage final : IsoMessage
{
  V5IsoMessage(EmulationKernel& ios, const IOCtlVRequest& cmd_buffer);
};
}  // namespace USB

// Shared part of the USBv5 resource managers (VEN and HIDv5).
class USBV5ResourceManager : public USBHost
{
public:
  using USBHost::USBHost;

  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override = 0;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override = 0;

  void DoState(PointerWrap& p) override;

protected:
  struct USBV5Device;
  // Looks up the entry named by the device ID block at in_buffer. Requires
  // m_usbv5_devices_mutex to be held.
  USBV5Device* GetUSBV5Device(u32 in_buffer);

  std::optional<IPCReply> GetDeviceChange(const IOCtlRequest& request);
  IPCReply SetAlternateSetting(USBV5Device& device, const IOCtlRequest& request);
  IPCReply Shutdown(const IOCtlRequest& request);
  IPCReply SuspendResume(USBV5Device& device, const IOCtlRequest& request);

  using Handler = std::function<std::optional<IPCReply>(USBV5Device&)>;
  // Checks the device ID block of an ioctl and runs `handler` on the device it names.
  std::optional<IPCReply> HandleDeviceIOCtl(const IOCtlRequest& request, const Handler& handler);
  IPCReply GetUSBVersion(const IOCtlRequest& request) const;

  void OnDeviceChange(ChangeEvent event, std::shared_ptr<USB::Device> device) override;
  void OnDeviceChangeEnd() override;
  // Answers the pending device-change request, or remembers that there is news for the next
  // one. Requires m_devicechange_hook_address_mutex to be held.
  void TriggerDeviceChangeReply();
  // HIDv5 puts the interface number into the first byte of each device ID; VEN does not.
  virtual bool HasInterfaceNumberInIDs() const = 0;

  bool m_has_pending_changes = true;
  std::mutex m_devicechange_hook_address_mutex;
  std::unique_ptr<IOCtlRequest> m_devicechange_hook_request;

  // IOS treats every interface of a USB device as a device of its own, and a USBv5 resource
  // manager has room for 32 of them. Saved as raw bytes in savestates.
  struct USBV5Device
  {
    bool in_use = false;
    u8 interface_number = 0;
    u16 number = 0;
    u64 host_id = 0;
  };
  std::array<USBV5Device, 32> m_usbv5_devices{};
  mutable std::mutex m_usbv5_devices_mutex;
  // Generation counter: entries created by the same device-change batch share a number, so a
  // stale ID from before a replug is rejected.
  u16 m_current_device_number = 0x21;
};
}  // namespace IOS::HLE
