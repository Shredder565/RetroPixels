// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>

#include "Common/CommonTypes.h"
#include "Core/HW/Triforce/IOPorts.h"
#include "Core/HW/Triforce/SerialDevice.h"

class PointerWrap;

namespace Triforce
{

// Force-feedback steering wheel of F-Zero AX (both cabinets), on serial port A.
// The game sends 4-byte requests; the servo target is fed back as the wheel
// position while the wheel calibrates.
class FZeroAXSteeringWheel final : public SerialDevice
{
public:
  void Update() override;

  bool IsInitializing() const;

  s8 GetServoPosition() const { return m_servo; }

  void DoState(PointerWrap& p) override;

private:
  void HandleRequest(std::span<const u8> request);

  u8 m_init_state = 0;
  s8 m_servo = 0;
};

// Buttons, pedals and lamps shared by both F-Zero AX cabinets.
class FZeroAXCommon_IOAdapter final : public IOAdapter
{
public:
  explicit FZeroAXCommon_IOAdapter(FZeroAXSteeringWheel* steering_wheel)
      : m_wheel{steering_wheel}
  {
  }

  void Update() override;

protected:
  void HandleGenericOutputsChanged(std::span<const u8> bits_set,
                                   std::span<const u8> bits_cleared) override;

private:
  FZeroAXSteeringWheel* const m_wheel;
};

// The Deluxe cabinet's moving seat.
class FZeroAXDeluxe_IOAdapter final : public IOAdapter
{
public:
  void Update() override;

  void DoState(PointerWrap& p) override;

private:
  u32 m_delay = 0;
  u8 m_rx_reply = 0xF0;
};

// The Monster Ride cabinet.
class FZeroAXMonster_IOAdapter final : public IOAdapter
{
public:
  void Update() override;
};

}  // namespace Triforce
