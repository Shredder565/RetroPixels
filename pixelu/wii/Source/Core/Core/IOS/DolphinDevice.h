// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Timer.h"
#include "Core/IOS/Device.h"

namespace IOS::HLE
{
// /dev/dolphin: an emulator-only device through which homebrew can query and adjust emulator
// settings (speed limit, version, Discord presence, host clock).
class DolphinDevice final : public EmulationDevice
{
public:
  DolphinDevice(EmulationKernel& ios, const std::string& device_name);
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

private:
  IPCReply GetElapsedTime(const IOCtlVRequest& request) const;
  IPCReply GetSystemTime(const IOCtlVRequest& request) const;

  Common::Timer m_timer;
};
}  // namespace IOS::HLE
