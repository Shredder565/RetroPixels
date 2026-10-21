// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>

#include "Common/CommonTypes.h"
#include "Core/HW/Triforce/IOPorts.h"
#include "Core/HW/Triforce/SerialDevice.h"

class PointerWrap;

namespace Triforce
{

// Force-feedback wheel of Mario Kart Arcade GP and GP2, on serial port A. It
// answers 10-byte force requests with a short ASCII handshake.
class MarioKartGPSteeringWheel final : public SerialDevice
{
public:
  void Update() override;

  void Reset();

  void DoState(PointerWrap& p) override;

private:
  void HandleRequest(std::span<const u8> request);

  u8 m_init_state = 0;
};

// Buttons, pedals and lamps of both Mario Kart Arcade GP cabinets.
class MarioKartGPCommon_IOAdapter final : public IOAdapter
{
public:
  explicit MarioKartGPCommon_IOAdapter(MarioKartGPSteeringWheel* steering_wheel)
      : m_wheel{steering_wheel}
  {
  }

  void Update() override;

protected:
  void HandleGenericOutputsChanged(std::span<const u8> bits_set,
                                   std::span<const u8> bits_cleared) override;

private:
  MarioKartGPSteeringWheel* const m_wheel;
};

}  // namespace Triforce
