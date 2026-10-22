// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// XInput is part of Windows since Windows 8. It is used through the DLL of the SDK we build
// with when present, else through the DirectX June 2010 redistributable's xinput1_3.dll.

#pragma once

#include <windows.h>
#include <xinput.h>

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#ifndef XINPUT_DEVSUBTYPE_FLIGHT_STICK
#error This needs the Windows SDK's xinput.h: drop any DirectX SDK (DXSDK_DIR) from the include path and check _WIN32_WINNT.
#endif

namespace ciface::XInput
{
// Loads the XInput DLL (once).
void Init();
// Replaces the XInput devices with the controllers connected now (up to four).
void PopulateDevices();
void DeInit();

// An XInput controller in one of the four user slots.
class Device final : public Core::Device
{
public:
  Device(const XINPUT_CAPABILITIES& capabilities, u8 index);

  std::string GetName() const override;
  std::string GetSource() const override;
  // The slot number.
  std::optional<int> GetPreferredId() const override;
  int GetSortPriority() const override { return -2; }

  Core::DeviceRemoval UpdateInput() override;

  // Sends the motor speeds.
  void UpdateMotors();

private:
  XINPUT_STATE m_state_in{};
  XINPUT_VIBRATION m_state_out{};
  ControlState m_battery_level{};
  const BYTE m_subtype;
  const u8 m_index;
};
}  // namespace ciface::XInput
