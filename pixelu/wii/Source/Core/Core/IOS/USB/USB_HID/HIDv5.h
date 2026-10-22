// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Host.h"
#include "Core/IOS/USB/USBV5.h"

namespace IOS::HLE
{
// /dev/usb/hid from IOS57 on (USB interface version 5).
class USB_HIDv5 final : public USBV5ResourceManager
{
public:
  using USBV5ResourceManager::USBV5ResourceManager;
  ~USB_HIDv5() override;

  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

private:
  IPCReply CancelEndpoint(USBV5Device& device, const IOCtlRequest& request);
  IPCReply GetDeviceInfo(USBV5Device& device, const IOCtlRequest& request);
  s32 SubmitTransfer(USBV5Device& device, USB::Device& host_device, const IOCtlVRequest& ioctlv);

  bool ShouldAddDevice(const USB::Device& device) const override;
  bool HasInterfaceNumberInIDs() const override { return true; }

  // The interrupt endpoints of each entry, learnt when the guest asks for its parameters.
  struct InterruptEndpoints
  {
    u8 in = 0;
    u8 out = 0;
  };
  InterruptEndpoints& EndpointsOf(const USBV5Device& device);

  std::array<InterruptEndpoints, 32> m_interrupt_endpoints{};
};
}  // namespace IOS::HLE
