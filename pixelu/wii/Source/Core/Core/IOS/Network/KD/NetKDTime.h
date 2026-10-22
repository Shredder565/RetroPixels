// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/IOS/Device.h"

namespace IOS::HLE
{
// /dev/net/kd/time: the WiiConnect24 clock. It runs off the emulated RTC, shifted by whatever
// offset the guest set with SET_UNIVERSAL_TIME.
class NetKDTimeDevice : public EmulationDevice
{
public:
  NetKDTimeDevice(EmulationKernel& ios, const std::string& device_name);
  ~NetKDTimeDevice() override;

  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;

  // The emulated time in seconds, including the guest's offset.
  u64 GetAdjustedUTC() const;

private:
  // Picks the offset so that GetAdjustedUTC returns `wii_utc` now.
  void SetAdjustedUTC(u64 wii_utc);

  // The emulated RTC as seconds (truncated to 32 bits), plus an hour to take off when the host
  // considers that moment to be in daylight saving time.
  struct ClockReading
  {
    u32 seconds;
    s64 dst_correction;
  };
  ClockReading ReadClock() const;

  u64 m_rtc_counter = 0;
  s64 m_offset = 0;
};
}  // namespace IOS::HLE
