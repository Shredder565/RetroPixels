// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"

namespace IOS::HLE
{
// Placeholder for a resource manager that is not emulated: it accepts Open, IOCtl and IOCtlV
// and answers every one of them with success, so titles that only probe it keep going.
class DeviceStub final : public Device
{
public:
  using Device::Device;
  std::optional<IPCReply> Open(const OpenRequest& request) override;
  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;
};
}  // namespace IOS::HLE
