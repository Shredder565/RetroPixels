// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/XInput/XInput.h"

#include <array>
#include <string>
#include <utility>

#ifndef XINPUT_GAMEPAD_GUIDE
#define XINPUT_GAMEPAD_GUIDE 0x0400
#endif

namespace ciface::XInput
{
namespace
{
// The functions used, loaded from the DLL.
struct Api
{
  decltype(&XInputGetCapabilities) get_capabilities = nullptr;
  decltype(&XInputSetState) set_state = nullptr;
  decltype(&XInputGetState) get_state = nullptr;
  decltype(&XInputGetBatteryInformation) get_battery_information = nullptr;
};

HMODULE s_library = nullptr;
Api s_api;
// The guide button is only reported by the undocumented XInputGetState at ordinal 100.
bool s_guide_reported = false;

struct ButtonName
{
  WORD mask;
  const char* name;
};

constexpr std::array<ButtonName, 15> BUTTONS{{
    {XINPUT_GAMEPAD_A, "Button A"},
    {XINPUT_GAMEPAD_B, "Button B"},
    {XINPUT_GAMEPAD_X, "Button X"},
    {XINPUT_GAMEPAD_Y, "Button Y"},
    {XINPUT_GAMEPAD_DPAD_UP, "Pad N"},
    {XINPUT_GAMEPAD_DPAD_DOWN, "Pad S"},
    {XINPUT_GAMEPAD_DPAD_LEFT, "Pad W"},
    {XINPUT_GAMEPAD_DPAD_RIGHT, "Pad E"},
    {XINPUT_GAMEPAD_START, "Start"},
    {XINPUT_GAMEPAD_BACK, "Back"},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, "Shoulder L"},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, "Shoulder R"},
    {XINPUT_GAMEPAD_GUIDE, "Guide"},
    {XINPUT_GAMEPAD_LEFT_THUMB, "Thumb L"},
    {XINPUT_GAMEPAD_RIGHT_THUMB, "Thumb R"},
}};

class ButtonInput final : public Core::Device::Input
{
public:
  ButtonInput(const WORD* buttons, const ButtonName& button) : m_buttons(buttons), m_button(button)
  {
  }

  std::string GetName() const override { return m_button.name; }
  ControlState GetState() const override { return (*m_buttons & m_button.mask) > 0; }

private:
  const WORD* const m_buttons;
  const ButtonName& m_button;
};

// A value divided by its full scale (negative for the lower half of a stick axis).
template <typename T>
class ScaledInput final : public Core::Device::Input
{
public:
  ScaledInput(const T* value, T full_scale, std::string name)
      : m_value(value), m_full_scale(full_scale), m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return ControlState(*m_value) / m_full_scale; }

private:
  const T* const m_value;
  const T m_full_scale;
  const std::string m_name;
};

class BatteryInput final : public Core::Device::Input
{
public:
  explicit BatteryInput(const ControlState* level) : m_level(level) {}

  std::string GetName() const override { return "Battery"; }
  bool IsDetectable() const override { return false; }
  ControlState GetState() const override { return *m_level; }

private:
  const ControlState* const m_level;
};

class MotorOutput final : public Core::Device::Output
{
public:
  MotorOutput(Device* device, WORD* speed, const char* name)
      : m_device(device), m_speed(speed), m_name(name)
  {
  }

  std::string GetName() const override { return m_name; }

  void SetState(ControlState state) override
  {
    const WORD speed = (WORD)(state * 65535);
    if (speed == *m_speed)
      return;
    *m_speed = speed;
    m_device->UpdateMotors();
  }

private:
  Device* const m_device;
  WORD* const m_speed;
  const char* const m_name;
};

template <typename F>
F LoadFunction(const char* name)
{
  return reinterpret_cast<F>(::GetProcAddress(s_library, name));
}
}  // namespace

void Init()
{
  if (s_library != nullptr)
    return;

  // The DLL of the SDK we build with (Windows 8 and later), else the DirectX June 2010 one.
  s_library = ::LoadLibrary(XINPUT_DLL);
  if (s_library == nullptr)
  {
    s_library = ::LoadLibrary(TEXT("xinput1_3.dll"));
    if (s_library == nullptr)
      return;
  }

  s_api.get_capabilities = LoadFunction<decltype(s_api.get_capabilities)>("XInputGetCapabilities");
  s_api.set_state = LoadFunction<decltype(s_api.set_state)>("XInputSetState");
  s_api.get_battery_information =
      LoadFunction<decltype(s_api.get_battery_information)>("XInputGetBatteryInformation");

  // Ordinal 100: XInputGetState with the guide button.
  s_api.get_state = LoadFunction<decltype(s_api.get_state)>(MAKEINTRESOURCEA(100));
  s_guide_reported = s_api.get_state != nullptr;
  if (s_api.get_state == nullptr)
    s_api.get_state = LoadFunction<decltype(s_api.get_state)>("XInputGetState");

  if (s_api.get_capabilities == nullptr || s_api.set_state == nullptr ||
      s_api.get_state == nullptr || s_api.get_battery_information == nullptr)
  {
    ::FreeLibrary(s_library);
    s_library = nullptr;
  }
}

void PopulateDevices()
{
  if (s_library == nullptr)
    return;

  g_controller_interface.RemoveDevice(
      [](const Core::Device* device) { return device->GetSource() == "XInput"; });

  XINPUT_CAPABILITIES capabilities;
  // XInput has four user slots.
  for (u8 slot = 0; slot != 4; ++slot)
  {
    if (s_api.get_capabilities(slot, 0, &capabilities) == ERROR_SUCCESS)
      g_controller_interface.AddDevice(std::make_shared<Device>(capabilities, slot));
  }
}

void DeInit()
{
  if (s_library == nullptr)
    return;

  ::FreeLibrary(s_library);
  s_library = nullptr;
}

Device::Device(const XINPUT_CAPABILITIES& capabilities, u8 index)
    : m_subtype(capabilities.SubType), m_index(index)
{
  // Some devices report wrong capabilities, so every control of a gamepad is offered.
  XINPUT_GAMEPAD& pad = m_state_in.Gamepad;

  for (const ButtonName& button : BUTTONS)
  {
    if (button.mask == XINPUT_GAMEPAD_GUIDE && !s_guide_reported)
      continue;
    AddInput(new ButtonInput(&pad.wButtons, button));
  }

  AddInput(new ScaledInput<BYTE>(&pad.bLeftTrigger, 255, "Trigger L"));
  AddInput(new ScaledInput<BYTE>(&pad.bRightTrigger, 255, "Trigger R"));

  const std::array<std::pair<const SHORT*, const char*>, 4> sticks{{
      {&pad.sThumbLX, "Left X"},
      {&pad.sThumbLY, "Left Y"},
      {&pad.sThumbRX, "Right X"},
      {&pad.sThumbRY, "Right Y"},
  }};
  for (const auto& [axis, name] : sticks)
  {
    AddInput(new ScaledInput<SHORT>(axis, -32768, std::string(name) + '-'));
    AddInput(new ScaledInput<SHORT>(axis, 32767, std::string(name) + '+'));
  }

  AddOutput(new MotorOutput(this, &m_state_out.wLeftMotorSpeed, "Motor L"));
  AddOutput(new MotorOutput(this, &m_state_out.wRightMotorSpeed, "Motor R"));

  AddInput(new BatteryInput(&m_battery_level));
}

std::string Device::GetName() const
{
  // Named after the controller kind it reports.
  static constexpr std::array<std::pair<BYTE, const char*>, 7> kinds{{
      {XINPUT_DEVSUBTYPE_GAMEPAD, "Gamepad"},
      {XINPUT_DEVSUBTYPE_WHEEL, "Wheel"},
      {XINPUT_DEVSUBTYPE_ARCADE_STICK, "Arcade Stick"},
      {XINPUT_DEVSUBTYPE_FLIGHT_STICK, "Flight Stick"},
      {XINPUT_DEVSUBTYPE_DANCE_PAD, "Dance Pad"},
      {XINPUT_DEVSUBTYPE_GUITAR, "Guitar"},
      {XINPUT_DEVSUBTYPE_DRUM_KIT, "Drum Kit"},
  }};
  for (const auto& [subtype, name] : kinds)
  {
    if (subtype == m_subtype)
      return name;
  }
  return "Device";
}

std::string Device::GetSource() const
{
  return "XInput";
}

std::optional<int> Device::GetPreferredId() const
{
  return m_index;
}

Core::DeviceRemoval Device::UpdateInput()
{
  s_api.get_state(m_index, &m_state_in);

  XINPUT_BATTERY_INFORMATION battery{};
  // (The result is an error code, not an HRESULT; any code but a few counts as success here.)
  if (FAILED(s_api.get_battery_information(m_index, BATTERY_DEVTYPE_GAMEPAD, &battery)))
    return Core::DeviceRemoval::Keep;

  switch (battery.BatteryType)
  {
  case BATTERY_TYPE_DISCONNECTED:
  case BATTERY_TYPE_UNKNOWN:
    m_battery_level = 0;
    break;
  case BATTERY_TYPE_WIRED:
    m_battery_level = BATTERY_INPUT_MAX_VALUE;
    break;
  default:
    m_battery_level =
        battery.BatteryLevel / ControlState(BATTERY_LEVEL_FULL) * BATTERY_INPUT_MAX_VALUE;
    break;
  }
  return Core::DeviceRemoval::Keep;
}

void Device::UpdateMotors()
{
  s_api.set_state(m_index, &m_state_out);
}
}  // namespace ciface::XInput
