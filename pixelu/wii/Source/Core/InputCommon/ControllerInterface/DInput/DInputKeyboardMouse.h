// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <windows.h>

#include "Common/Matrix.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"
#include "InputCommon/ControllerInterface/DInput/DInput8.h"

namespace ciface::DInput
{
// Adds the keyboard and mouse device unless it exists already. `hwnd` is the window the
// cursor position is measured in.
void InitKeyboardMouse(IDirectInput8* const idi8, HWND hwnd);
// Changes the window the cursor position is measured in.
void SetKeyboardMouseWindow(HWND hwnd);

using RelativeMouseState = RelativeInputState<Common::TVec3<LONG>>;

// The system keyboard and mouse as a single device, so that combinations such as shift+click
// can be mapped. Both are DirectInput "system" devices, present even with nothing plugged in.
class KeyboardMouse : public Core::Device
{
public:
  KeyboardMouse(const LPDIRECTINPUTDEVICE8 kb_device, const LPDIRECTINPUTDEVICE8 mo_device);
  ~KeyboardMouse() override;

  Core::DeviceRemoval UpdateInput() override;

  std::string GetName() const override;
  std::string GetSource() const override;
  int GetSortPriority() const override;
  bool IsVirtualDevice() const override;

private:
  // The layout matters: mouse axes past the third read on into the following bytes (see the
  // constructor), as they always have.
  struct State
  {
    BYTE keyboard[256]{};
    // Mouse buttons and movement; the movement is smoothed over updates.
    DIMOUSESTATE2 mouse{};
    // Cursor position in the window, -1 to 1 (scaled by the window input scale).
    Common::TVec2<ControlState> cursor;
    // Unsmoothed movement since the previous update, per input channel.
    RelativeMouseState relative_mouse;
  };

  void ReadCursor();
  void ReadMouse();
  void ReadKeyboard();

  const LPDIRECTINPUTDEVICE8 m_kb_device;
  const LPDIRECTINPUTDEVICE8 m_mo_device;

  // GetTickCount() at the previous update.
  DWORD m_last_update;
  State m_state_in;
};
}  // namespace ciface::DInput
