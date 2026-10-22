// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/ForceFeedback/ForceFeedbackDevice.h"

namespace ciface::DInput
{
// Adds the attached game controllers that are neither XInput devices nor in use already.
void InitJoystick(IDirectInput8* const idi8, HWND hwnd);

// A DirectInput game controller, read through the standard DIJOYSTATE format: up to 32
// buttons, 4 hats, 6 axes and 2 sliders.
class Joystick : public ForceFeedback::ForceFeedbackDevice
{
public:
  explicit Joystick(const LPDIRECTINPUTDEVICE8 device);
  ~Joystick() override;

  Core::DeviceRemoval UpdateInput() override;

  std::string GetName() const override;
  std::string GetSource() const override;
  int GetSortPriority() const override { return -3; }

  // A joystick that can no longer be acquired has been unplugged.
  bool IsValid() const final;

private:
  void ApplyBufferedEvents(const DIDEVICEOBJECTDATA* events, DWORD count);

  const LPDIRECTINPUTDEVICE8 m_device;
  DIJOYSTATE m_state_in{};
  // Whether changes are read as buffered events rather than whole states.
  bool m_buffered;
};
}  // namespace ciface::DInput
