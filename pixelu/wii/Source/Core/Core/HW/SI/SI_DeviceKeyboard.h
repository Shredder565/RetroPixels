// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Core/HW/SI/SI_Device.h"

class PointerWrap;
struct KeyboardStatus;

namespace SerialInterface
{
// The GameCube ASCII keyboard (Phantasy Star Online). Each poll reports up
// to three held keys as scancodes plus a 4-bit sequence counter.
class CSIDevice_Keyboard final : public ISIDevice
{
public:
  CSIDevice_Keyboard(Core::System& system, SIDevices device, int device_number);

  int RunBuffer(u8* buffer, int request_length) override;

  DataResponse GetData(u32& hi, u32& low) override;

  KeyboardStatus GetKeyboardStatus() const;

  void SendCommand(u32 command, u8 poll) override;

  void DoState(PointerWrap& p) override;

private:
  using KeyArray = std::array<u8, 3>;

  KeyArray MapKeys(const KeyboardStatus& key_status) const;

  // Advanced by every poll command, wraps at 16.
  u8 m_counter = 0;
};
}  // namespace SerialInterface
