// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/DInput/DInputKeyboardMouse.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <string>

#include <fmt/format.h>

#include "Common/Logging/Log.h"

#include "Core/Core.h"
#include "Core/Host.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DInput/DInput.h"
#include "InputCommon/ControllerInterface/DInput/NamedKeys.h"

namespace ciface::DInput
{
namespace
{
// Mouse movement (in mickeys per update, smoothed) that counts as a full axis; the wheel uses
// one notch. Lower would be more sensitive; users can scale it down further with the range.
constexpr LONG MOUSE_FULL_MOVEMENT = 8;
constexpr LONG WHEEL_FULL_MOVEMENT = 1;

// After this long (ms) without an update, the mouse state is reset and one reading skipped,
// as it would otherwise hold the whole pile-up of movement.
constexpr DWORD STALE_MOUSE_TIME = 250;

// There is only ever one keyboard and mouse device. Accessed from several threads.
std::atomic<bool> s_keyboard_mouse_exists = false;
std::atomic<HWND> s_window = nullptr;

// A key or a mouse button: pressed when its state byte is nonzero.
class StateByte final : public Core::Device::Input
{
public:
  StateByte(const BYTE* state, std::string name) : m_state(state), m_name(std::move(name)) {}

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return *m_state != 0; }

private:
  const BYTE* const m_state;
  const std::string m_name;
};

// One direction of (smoothed) mouse movement along an axis, the wheel being the third.
class MouseAxis final : public Core::Device::Input
{
public:
  MouseAxis(u8 index, const LONG* movement, LONG full_movement)
      : m_movement(movement), m_full_movement(full_movement), m_index(index)
  {
  }

  std::string GetName() const override
  {
    return fmt::format("Axis {}{}", char('X' + m_index), m_full_movement < 0 ? '-' : '+');
  }
  bool IsDetectable() const override { return false; }
  ControlState GetState() const override { return ControlState(*m_movement) / m_full_movement; }

private:
  const LONG* const m_movement;
  const LONG m_full_movement;
  const u8 m_index;
};

// One direction of the cursor position, measured from the window centre.
class CursorAxis final : public Core::Device::Input
{
public:
  CursorAxis(u8 index, const ControlState* position, bool positive)
      : m_position(position), m_index(index), m_positive(positive)
  {
  }

  std::string GetName() const override
  {
    return fmt::format("Cursor {}{}", char('X' + m_index), m_positive ? '+' : '-');
  }
  bool IsDetectable() const override { return false; }
  ControlState GetState() const override { return *m_position / (m_positive ? 1.0 : -1.0); }

private:
  const ControlState* const m_position;
  const u8 m_index;
  const bool m_positive;
};

// One direction of raw mouse movement since the previous update.
class RelativeMouseAxis final : public Core::Device::RelativeInput
{
public:
  RelativeMouseAxis(u8 index, bool positive, const RelativeMouseState* state)
      : m_state(state), m_index(index), m_sign(positive ? 1 : -1)
  {
  }

  std::string GetName() const override
  {
    return fmt::format("RelativeMouse {}{}", char('X' + m_index), m_sign > 0 ? '+' : '-');
  }
  ControlState GetState() const override
  {
    return ControlState(m_state->GetValue().data[m_index] * m_sign);
  }

private:
  const RelativeMouseState* const m_state;
  const u8 m_index;
  const s8 m_sign;
};

bool IsLost(HRESULT result)
{
  return result == DIERR_INPUTLOST || result == DIERR_NOTACQUIRED;
}
}  // namespace

void InitKeyboardMouse(IDirectInput8* const idi8, HWND hwnd)
{
  if (s_keyboard_mouse_exists)
    return;

  s_window = hwnd;

  LPDIRECTINPUTDEVICE8 keyboard = nullptr;
  LPDIRECTINPUTDEVICE8 mouse = nullptr;
  constexpr DWORD shared_access = DISCL_BACKGROUND | DISCL_NONEXCLUSIVE;
  const bool created = SUCCEEDED(idi8->CreateDevice(GUID_SysKeyboard, &keyboard, nullptr)) &&
                       SUCCEEDED(keyboard->SetDataFormat(&c_dfDIKeyboard)) &&
                       SUCCEEDED(keyboard->SetCooperativeLevel(nullptr, shared_access)) &&
                       SUCCEEDED(idi8->CreateDevice(GUID_SysMouse, &mouse, nullptr)) &&
                       SUCCEEDED(mouse->SetDataFormat(&c_dfDIMouse2)) &&
                       SUCCEEDED(mouse->SetCooperativeLevel(nullptr, shared_access));
  if (created)
  {
    g_controller_interface.AddDevice(std::make_shared<KeyboardMouse>(keyboard, mouse));
    return;
  }

  ERROR_LOG_FMT(CONTROLLERINTERFACE, "KeyboardMouse device failed to be created");
  if (keyboard != nullptr)
    keyboard->Release();
  if (mouse != nullptr)
    mouse->Release();
}

void SetKeyboardMouseWindow(HWND hwnd)
{
  s_window = hwnd;
}

KeyboardMouse::KeyboardMouse(const LPDIRECTINPUTDEVICE8 kb_device,
                             const LPDIRECTINPUTDEVICE8 mo_device)
    : m_kb_device(kb_device), m_mo_device(mo_device), m_last_update(GetTickCount()), m_state_in{}
{
  s_keyboard_mouse_exists = true;

  // Acquiring can fail now (e.g. without focus); it is retried on updates.
  if (FAILED(m_kb_device->Acquire()))
    WARN_LOG_FMT(CONTROLLERINTERFACE, "Keyboard device failed to acquire. We'll retry later");
  if (FAILED(m_mo_device->Acquire()))
    WARN_LOG_FMT(CONTROLLERINTERFACE, "Mouse device failed to acquire. We'll retry later");

  for (const NamedKey& key : NAMED_KEYS)
    AddInput(new StateByte(&m_state_in.keyboard[key.code], key.name));

  // Either-side modifiers, named as on the other platforms.
  AddCombinedInput("Alt", {"LMENU", "RMENU"});
  AddCombinedInput("Shift", {"LSHIFT", "RSHIFT"});
  AddCombinedInput("Ctrl", {"LCONTROL", "RCONTROL"});

  DIDEVCAPS mouse_caps{};
  mouse_caps.dwSize = sizeof(mouse_caps);
  m_mo_device->GetCapabilities(&mouse_caps);

  // All eight buttons of the data format; the reported button count is not reliable.
  for (std::size_t i = 0; i != std::size(m_state_in.mouse.rgbButtons); ++i)
  {
    AddInput(new StateByte(&m_state_in.mouse.rgbButtons[i], std::string("Click ") + char('0' + i)));
  }

  // The axes follow each other in the state: X, Y, wheel. (A mouse claiming more axes gets
  // inputs reading the bytes after them.)
  for (DWORD i = 0; i < mouse_caps.dwAxes; ++i)
  {
    const LONG* const movement = &m_state_in.mouse.lX + i;
    const LONG full = i == 2 ? WHEEL_FULL_MOVEMENT : MOUSE_FULL_MOVEMENT;
    AddInput(new MouseAxis(u8(i), movement, -full));
    AddInput(new MouseAxis(u8(i), movement, full));
  }

  AddInput(new CursorAxis(0, &m_state_in.cursor.x, false));
  AddInput(new CursorAxis(0, &m_state_in.cursor.x, true));
  AddInput(new CursorAxis(1, &m_state_in.cursor.y, false));
  AddInput(new CursorAxis(1, &m_state_in.cursor.y, true));

  for (DWORD i = 0; i != mouse_caps.dwAxes; ++i)
  {
    AddInput(new RelativeMouseAxis(u8(i), false, &m_state_in.relative_mouse));
    AddInput(new RelativeMouseAxis(u8(i), true, &m_state_in.relative_mouse));
  }
}

KeyboardMouse::~KeyboardMouse()
{
  s_keyboard_mouse_exists = false;

  // (A debugger break in here can leave dinput8.dll spinning forever on the next device
  // refresh; this does not happen otherwise.)
  m_kb_device->Unacquire();
  m_kb_device->Release();
  m_mo_device->Unacquire();
  m_mo_device->Release();
}

Core::DeviceRemoval KeyboardMouse::UpdateInput()
{
  ReadCursor();
  ReadMouse();
  ReadKeyboard();
  return Core::DeviceRemoval::Keep;
}

void KeyboardMouse::ReadCursor()
{
  const HWND window = s_window;

  // The window can have a zero size.
  RECT client;
  GetClientRect(window, &client);
  const LONG width = std::max(client.right - client.left, LONG{1});
  const LONG height = std::max(client.bottom - client.top, LONG{1});

  POINT position{};
  if (g_controller_interface.IsMouseCenteringRequested() &&
      (Host_RendererHasFocus() || Host_TASInputHasFocus()))
  {
    position.x = width / 2;
    position.y = height / 2;

    POINT on_screen = position;
    ClientToScreen(window, &on_screen);
    SetCursorPos(on_screen.x, on_screen.y);
    g_controller_interface.SetMouseCenteringRequested(false);
  }
  else if (Host_TASInputHasFocus())
  {
    // With a TAS input window focused, input is read as if the game window had focus, except
    // for the cursor: clicking around the TAS window must not move the pointer in the game.
    return;
  }
  else
  {
    GetCursorPos(&position);
    ScreenToClient(window, &position);
  }

  const Common::Vec2 scale = g_controller_interface.GetWindowInputScale();
  m_state_in.cursor.x = (ControlState(position.x) / width * 2 - 1) * scale.x;
  m_state_in.cursor.y = (ControlState(position.y) / height * 2 - 1) * scale.y;
}

void KeyboardMouse::ReadMouse()
{
  DIMOUSESTATE2 reading{};

  const DWORD now = GetTickCount();
  if (now - m_last_update > STALE_MOUSE_TIME)
  {
    m_state_in.mouse = {};
    m_state_in.relative_mouse = {};
    m_mo_device->GetDeviceState(sizeof(reading), &reading);
  }
  m_last_update = now;

  const HRESULT result = m_mo_device->GetDeviceState(sizeof(reading), &reading);
  if (IsLost(result))
  {
    INFO_LOG_FMT(CONTROLLERINTERFACE, "Mouse device failed to get state");
    if (FAILED(m_mo_device->Acquire()))
      INFO_LOG_FMT(CONTROLLERINTERFACE, "Mouse device failed to re-acquire, we'll retry later");
    return;
  }
  if (FAILED(result))
    return;

  m_state_in.relative_mouse.Move({reading.lX, reading.lY, reading.lZ});
  m_state_in.relative_mouse.Update();

  // Movement is averaged with the previous value; it is too jumpy to use otherwise.
  LONG* const smoothed = &m_state_in.mouse.lX;
  const LONG* const latest = &reading.lX;
  for (int axis = 0; axis != 3; ++axis)
    smoothed[axis] = (smoothed[axis] + latest[axis]) / 2;

  std::ranges::copy(reading.rgbButtons, m_state_in.mouse.rgbButtons);
}

void KeyboardMouse::ReadKeyboard()
{
  const HRESULT result =
      m_kb_device->GetDeviceState(sizeof(m_state_in.keyboard), &m_state_in.keyboard);
  if (!IsLost(result))
    return;

  INFO_LOG_FMT(CONTROLLERINTERFACE, "Keyboard device failed to get state");
  if (FAILED(m_kb_device->Acquire()))
  {
    INFO_LOG_FMT(CONTROLLERINTERFACE, "Keyboard device failed to re-acquire, we'll retry later");
    return;
  }
  m_kb_device->GetDeviceState(sizeof(m_state_in.keyboard), &m_state_in.keyboard);
}

std::string KeyboardMouse::GetName() const
{
  return "Keyboard Mouse";
}

std::string KeyboardMouse::GetSource() const
{
  return DINPUT_SOURCE_NAME;
}

// First in the device list.
int KeyboardMouse::GetSortPriority() const
{
  return DEFAULT_DEVICE_SORT_PRIORITY;
}

bool KeyboardMouse::IsVirtualDevice() const
{
  return true;
}
}  // namespace ciface::DInput
