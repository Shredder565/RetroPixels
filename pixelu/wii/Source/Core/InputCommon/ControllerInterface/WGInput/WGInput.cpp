// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/WGInput/WGInput.h"

#include <array>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// CoGetApartmentType
#include <objbase.h>
#pragma comment(lib, "ole32")

// WinRT reports failures as winrt::hresult_error exceptions. Every call is guarded so that
// none escapes into the rest of the program.
#include <winrt/base.h>
#include <winrt/windows.devices.haptics.h>
#include <winrt/windows.devices.power.h>
#include <winrt/windows.foundation.collections.h>
#include <winrt/windows.gaming.input.h>
#include <winrt/windows.system.power.h>
#pragma comment(lib, "windowsapp")

#include <fmt/format.h>

#include "Common/HRWrap.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/WorkQueueThread.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace ciface::WGInput
{
namespace
{
namespace WGI = winrt::Windows::Gaming::Input;
namespace Haptics = winrt::Windows::Devices::Haptics;

constexpr std::string_view SOURCE_NAME = "WGInput";

// Input names for the GameControllerButtonLabel values, by value ("None" is never used).
// Xbox and generic labels of the same control share a name; no device is expected to have
// both.
constexpr std::array<const char*, 69> BUTTON_LABEL_NAMES{
    "None",
    // Xbox: Back, Start, Menu, View, d-pad, A, B, X, Y, left and right bumper, trigger and
    // stick button, paddles.
    "Back", "Start", "Menu", "View", "Pad N", "Pad S", "Pad W", "Pad E", "Button A", "Button B",
    "Button X", "Button Y", "Bumper L", "Trigger L", "Thumb L", "Bumper R", "Trigger R", "Thumb R",
    "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4",
    // Generic: Mode, Select, Menu, View, Back, Start, Options, Share, d-pad.
    "Mode", "Select", "Menu", "View", "Back", "Start", "Options", "Share", "Pad N", "Pad S",
    "Pad W", "Pad E",
    // Letters.
    "Letter A", "Letter B", "Letter C", "Letter L", "Letter R", "Letter X", "Letter Y", "Letter Z",
    // PlayStation style.
    "Cross", "Circle", "Square", "Triangle",
    // Left and right bumper, trigger, stick button, 1, 2, 3.
    "Bumper L", "Trigger L", "Thumb L", "Left 1", "Left 2", "Left 3", "Bumper R", "Trigger R",
    "Thumb R", "Right 1", "Right 2", "Right 3",
    // Paddles and the rest.
    "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Plus", "Minus", "Down Left Arrow", "Dial L",
    "Dial R", "Suspension"};

class Device;

// Pressed while its byte (one of the raw controller's button values) is nonzero.
class ButtonInput final : public Core::Device::Input
{
public:
  ButtonInput(const u8* state, std::string name) : m_state(state), m_name(std::move(name)) {}

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return ControlState(*m_state != 0); }

private:
  const u8* const m_state;
  const std::string m_name;
};

// (value - base) / range.
class AxisInput final : public Core::Device::Input
{
public:
  AxisInput(const double* value, double base, double range, std::string name)
      : m_value(value), m_base(base), m_range(range), m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return ControlState(*m_value - m_base) / m_range; }

private:
  const double* const m_value;
  const double m_base;
  const double m_range;
  const std::string m_name;
};

// One direction of a switch (hat). Diagonal positions count for both neighbours.
class SwitchInput final : public Core::Device::Input
{
public:
  SwitchInput(const WGI::GameControllerSwitchPosition* position, u32 index,
              WGI::GameControllerSwitchPosition direction)
      : m_position(position), m_direction(static_cast<int32_t>(direction)),
        m_name(fmt::format("Switch {} {}", index, "NESW"[m_direction / 2]))
  {
  }

  std::string GetName() const override { return m_name; }

  ControlState GetState() const override
  {
    if (*m_position == WGI::GameControllerSwitchPosition::Center)
      return 0.0;

    // The eight non-centre positions go clockwise from Up (1) to UpLeft (8).
    const int32_t distance = std::abs(static_cast<int32_t>(*m_position) - m_direction);
    return ControlState(distance <= 1 || distance == 7);
  }

private:
  const WGI::GameControllerSwitchPosition* const m_position;
  const int32_t m_direction;
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

// A haptic feedback (waveform) of a SimpleHapticsController, played at the output's strength.
class HapticOutput final : public Core::Device::Output
{
public:
  HapticOutput(Haptics::SimpleHapticsController controller,
               Haptics::SimpleHapticsControllerFeedback feedback, std::string name)
      : m_controller(controller), m_feedback(feedback), m_name(std::move(name))
  {
  }

  std::string GetName() const override { return m_name; }

  void SetState(ControlState state) override
  {
    if (state == m_state)
      return;
    m_state = state;

    try
    {
      if (state != 0)
        m_controller.SendHapticFeedback(m_feedback, state);
      else
        m_controller.StopFeedback();
    }
    catch (winrt::hresult_error)
    {
    }
  }

private:
  Haptics::SimpleHapticsController m_controller;
  Haptics::SimpleHapticsControllerFeedback m_feedback;
  const std::string m_name;
  ControlState m_state = 0;
};

class Device final : public Core::Device
{
public:
  Device(std::string name, WGI::RawGameController raw, WGI::Gamepad gamepad);

  std::string GetName() const override { return m_name; }
  std::string GetSource() const override { return std::string(SOURCE_NAME); }
  int GetSortPriority() const override { return -1; }

  Core::DeviceRemoval UpdateInput() override;

  // (Returns a new reference.)
  WGI::RawGameController RawController() const { return m_raw; }

  // Sends the gamepad's vibration levels.
  void SendVibration();

private:
  void AddButtons();
  void AddGamepadControls();
  void AddRawAxes();
  void AddSwitches();
  void AddHaptics();
  bool ReadBattery();

  const std::string m_name;
  const WGI::RawGameController m_raw;
  WGI::Gamepad m_gamepad = nullptr;

  // Raw controller readings. The buttons are bytes (rather than a bit-packed vector<bool>)
  // so that they can be handed over as an array of bool.
  std::vector<u8> m_buttons;
  std::vector<WGI::GameControllerSwitchPosition> m_switches;
  std::vector<double> m_axes;

  // Gamepad reading and vibration.
  WGI::GamepadReading m_gamepad_reading{};
  WGI::GamepadVibration m_vibration{};

  ControlState m_battery_level = 0;
};

// One of the gamepad's four vibration levels.
class VibrationOutput final : public Core::Device::Output
{
public:
  VibrationOutput(Device* device, double* level, std::string_view name)
      : m_device(device), m_level(level), m_name(name)
  {
  }

  std::string GetName() const override { return std::string(m_name); }

  void SetState(ControlState state) override
  {
    if (*m_level == state)
      return;
    *m_level = state;
    m_device->SendVibration();
  }

private:
  Device* const m_device;
  double* const m_level;
  const std::string_view m_name;
};

Device::Device(std::string name, WGI::RawGameController raw, WGI::Gamepad gamepad)
    : m_name(std::move(name)), m_raw(raw), m_gamepad(gamepad)
{
  AddButtons();

  // With the Gamepad interface its axes are used, else the raw controller's.
  if (m_gamepad)
    AddGamepadControls();
  else
    AddRawAxes();

  // Switches come from the raw controller in any case: some gamepads (DualShock 4) have
  // their d-pad only there.
  AddSwitches();

  AddHaptics();

  if (ReadBattery())
    AddInput(new BatteryInput(&m_battery_level));
}

void Device::AddButtons()
{
  try
  {
    m_buttons.resize(static_cast<std::size_t>(m_raw.ButtonCount()));
    for (u32 index = 0; index != m_buttons.size(); ++index)
    {
      auto label = WGI::GameControllerButtonLabel::None;
      try
      {
        label = m_raw.GetButtonLabel(static_cast<int32_t>(index));
      }
      catch (winrt::hresult_error)
      {
        label = WGI::GameControllerButtonLabel::None;
      }

      // Labelled buttons are named after the label, the others by number.
      const auto label_index = static_cast<std::size_t>(static_cast<int32_t>(label));
      if (label != WGI::GameControllerButtonLabel::None && label_index < BUTTON_LABEL_NAMES.size())
        AddInput(new ButtonInput(&m_buttons[index], BUTTON_LABEL_NAMES[label_index]));
      else
        AddInput(new ButtonInput(&m_buttons[index], fmt::format("Button {}", index)));
    }
  }
  catch (winrt::hresult_error)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Error populating buttons");
  }
}

void Device::AddGamepadControls()
{
  WGI::GamepadReading& reading = m_gamepad_reading;

  const std::array<std::pair<const double*, const char*>, 4> sticks{{
      {&reading.LeftThumbstickX, "Left X"},
      {&reading.LeftThumbstickY, "Left Y"},
      {&reading.RightThumbstickX, "Right X"},
      {&reading.RightThumbstickY, "Right Y"},
  }};
  for (const auto& [value, name] : sticks)
  {
    AddInput(new AxisInput(value, 0.0, -1.0, std::string(name) + '-'));
    AddInput(new AxisInput(value, 0.0, +1.0, std::string(name) + '+'));
  }

  AddInput(new AxisInput(&reading.LeftTrigger, 0.0, 1.0, "Trigger L"));
  AddInput(new AxisInput(&reading.RightTrigger, 0.0, 1.0, "Trigger R"));

  AddOutput(new VibrationOutput(this, &m_vibration.LeftMotor, "Motor L"));
  AddOutput(new VibrationOutput(this, &m_vibration.RightMotor, "Motor R"));
  AddOutput(new VibrationOutput(this, &m_vibration.LeftTrigger, "Trigger L"));
  AddOutput(new VibrationOutput(this, &m_vibration.RightTrigger, "Trigger R"));
}

void Device::AddRawAxes()
{
  try
  {
    m_axes.resize(static_cast<std::size_t>(m_raw.AxisCount()));
    // Raw axes go from 0 to 1. (The "low" half here is the one named +.)
    for (u32 index = 0; index != m_axes.size(); ++index)
    {
      const double* const value = &m_axes[index];
      AddFullAnalogSurfaceInputs(new AxisInput(value, 0.5, +0.5, fmt::format("Axis {}+", index)),
                                 new AxisInput(value, 0.5, -0.5, fmt::format("Axis {}-", index)));
    }
  }
  catch (winrt::hresult_error)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Error populating axes");
  }
}

void Device::AddSwitches()
{
  try
  {
    m_switches.resize(static_cast<std::size_t>(m_raw.SwitchCount()));

    // All kinds are read before adding inputs, so that a failure adds none.
    std::vector<WGI::GameControllerSwitchKind> kinds;
    for (u32 index = 0; index < m_switches.size(); ++index)
      kinds.push_back(m_raw.GetSwitchKind(static_cast<int32_t>(index)));

    using Position = WGI::GameControllerSwitchPosition;
    for (u32 index = 0; index != m_switches.size(); ++index)
    {
      const Position* const position = &m_switches[index];
      AddInput(new SwitchInput(position, index, Position::Up));
      AddInput(new SwitchInput(position, index, Position::Down));
      // A two-way switch only goes up and down.
      if (kinds[index] != WGI::GameControllerSwitchKind::TwoWay)
      {
        AddInput(new SwitchInput(position, index, Position::Left));
        AddInput(new SwitchInput(position, index, Position::Right));
      }
    }
  }
  catch (winrt::hresult_error)
  {
    m_switches.clear();
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Error populating switches");
  }
}

void Device::AddHaptics()
{
  // The waveforms offered, by their waveform ID.
  static const std::map<uint16_t, std::string> waveform_names{
      {Haptics::KnownSimpleHapticsControllerWaveforms::Click(), "Click"},
      {Haptics::KnownSimpleHapticsControllerWaveforms::BuzzContinuous(), "Buzz"},
      {Haptics::KnownSimpleHapticsControllerWaveforms::RumbleContinuous(), "Rumble"},
  };

  // (SimpleHapticsControllers exists from Windows 10 1709.) A failure keeps the outputs added
  // so far.
  try
  {
    u32 controller_index = 0;
    for (const Haptics::SimpleHapticsController controller : m_raw.SimpleHapticsControllers())
    {
      for (const Haptics::SimpleHapticsControllerFeedback feedback : controller.SupportedFeedback())
      {
        const uint16_t waveform = feedback.Waveform();
        const auto known = waveform_names.find(waveform);
        if (known == waveform_names.end())
        {
          WARN_LOG_FMT(CONTROLLERINTERFACE,
                       "WGInput: Unhandled haptics feedback waveform: 0x{:04x}.", waveform);
          continue;
        }
        AddOutput(new HapticOutput(controller, feedback,
                                   fmt::format("{} {}", known->second, controller_index)));
      }
      ++controller_index;
    }
  }
  catch (winrt::hresult_error)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Error populating haptics");
  }
}

Core::DeviceRemoval Device::UpdateInput()
{
  // The bytes of m_buttons, seen as the bools GetCurrentReading wants.
  static_assert(sizeof(bool) == sizeof(u8));
  const winrt::array_view<bool> buttons(
      reinterpret_cast<bool*>(m_buttons.data()),
      static_cast<winrt::array_view<bool>::size_type>(m_buttons.size()));
  try
  {
    m_raw.GetCurrentReading(buttons, m_switches, m_axes);
  }
  catch (winrt::hresult_error error)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: IRawGameController::GetCurrentReading failed: {}",
                  error.code());
  }

  if (m_gamepad)
  {
    try
    {
      m_gamepad_reading = m_gamepad.GetCurrentReading();
    }
    catch (winrt::hresult_error error)
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: IGamepad::GetCurrentReading failed: {}",
                    error.code());
    }
  }

  if (!ReadBattery())
    DEBUG_LOG_FMT(CONTROLLERINTERFACE, "WGInput: UpdateBatteryLevel failed.");

  return Core::DeviceRemoval::Keep;
}

void Device::SendVibration()
{
  try
  {
    m_gamepad.Vibration(m_vibration);
  }
  catch (winrt::hresult_error)
  {
  }
}

bool Device::ReadBattery()
{
  using winrt::Windows::System::Power::BatteryStatus;

  try
  {
    // No battery information while Steam's overlay (GameOverlayRenderer64.dll) is loaded.
    const auto battery = m_raw.try_as<WGI::IGameControllerBatteryInfo>();
    if (!battery)
      return false;
    // (Each report leaks a little memory, at least up to Windows 21H2.)
    const winrt::Windows::Devices::Power::BatteryReport report = battery.TryGetBatteryReport();
    if (!report)
      return false;

    switch (report.Status())
    {
    case BatteryStatus::NotPresent:
      m_battery_level = 0;
      return true;
    case BatteryStatus::Idle:
    case BatteryStatus::Charging:
      m_battery_level = BATTERY_INPUT_MAX_VALUE;
      return true;
    default:
      break;
    }

    const int32_t full = report.FullChargeCapacityInMilliwattHours().GetInt32();
    const int32_t remaining = report.RemainingCapacityInMilliwattHours().GetInt32();
    m_battery_level = BATTERY_INPUT_MAX_VALUE * remaining / full;
    return true;
  }
  catch (winrt::hresult_error)
  {
    return false;
  }
}

// ----- Device list -----

enum class ChangeKind
{
  AddOrReplace,
  Remove,
};

struct ControllerChange
{
  ChangeKind kind;
  WGI::RawGameController controller;
};

// Whether this thread initialised WinRT in Init().
thread_local bool s_owns_winrt;
winrt::event_token s_added_token;
winrt::event_token s_removed_token;
// Controller changes, reported on WinRT's thread pool, are handled on this thread.
Common::WorkQueueThread<ControllerChange> s_change_queue;

bool IsComInitialized()
{
  APTTYPE type{};
  APTTYPEQUALIFIER qualifier{};
  return CoGetApartmentType(&type, &qualifier) == S_OK;
}

void RemoveController(const WGI::RawGameController& controller)
{
  g_controller_interface.RemoveDevice([&controller](const Core::Device* device) {
    return device->GetSource() == SOURCE_NAME &&
           static_cast<const Device*>(device)->RawController() == controller;
  });
}

void AddController(const WGI::RawGameController& controller)
{
  // The display name, or failing that one made from the USB IDs.
  std::string name;
  try
  {
    name = StripWhitespace(WStringToUTF8(controller.DisplayName().c_str()));
    if (name.empty())
    {
      const u16 vendor = controller.HardwareVendorId();
      const u16 product = controller.HardwareProductId();
      name = fmt::format("Device_{:04x}:{:04x}", vendor, product);
      INFO_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Empty device name, using {}", name);
    }
  }
  catch (winrt::hresult_error)
  {
    name = "Device";
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Failed to get device name, using {}", name);
  }

  try
  {
    // Not every controller is a gamepad; the device copes with a null one.
    const WGI::Gamepad gamepad = WGI::Gamepad::FromGameController(controller);
    auto device = std::make_shared<Device>(std::move(name), controller, gamepad);
    if (!device->Inputs().empty() || !device->Outputs().empty())
      g_controller_interface.AddDevice(std::move(device));
  }
  catch (winrt::hresult_error)
  {
    // (`name` may have been moved into the device by now.)
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Failed to add device {}", name);
  }
}

void HandleChange(ControllerChange change)
{
  try
  {
    winrt::init_apartment();
  }
  catch (const winrt::hresult_error& error)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE,
                  "WGInput: Failed to CoInitialize for add/remove controller event: {}",
                  WStringToUTF8(error.message()));
    return;
  }
  Common::ScopeGuard uninit([] { winrt::uninit_apartment(); });

  switch (change.kind)
  {
  case ChangeKind::AddOrReplace:
    RemoveController(change.controller);
    AddController(change.controller);
    break;
  case ChangeKind::Remove:
    RemoveController(change.controller);
    break;
  default:
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Invalid add/remove controller event: {}",
                  std::to_underlying(change.kind));
    break;
  }
}
}  // namespace

#pragma warning(push)
// C4265 on WinRT's delegate wrapper for the lambdas below (virtual functions without a
// virtual destructor).
#pragma warning(disable : 4265)

void Init()
{
  // Devices may only be touched from threads with WinRT (COM) initialised.
  if (!IsComInitialized())
  {
    winrt::init_apartment();
    s_owns_winrt = true;
  }

  s_change_queue.Reset("WGInput Add/Remove Device Thread", HandleChange);

  try
  {
    s_added_token = WGI::RawGameController::RawGameControllerAdded(
        [](auto&&, WGI::RawGameController controller) {
          s_change_queue.EmplaceItem(
              ControllerChange{ChangeKind::AddOrReplace, std::move(controller)});
        });
    s_removed_token = WGI::RawGameController::RawGameControllerRemoved(
        [](auto&&, WGI::RawGameController controller) {
          s_change_queue.EmplaceItem(ControllerChange{ChangeKind::Remove, std::move(controller)});
        });
  }
  catch (winrt::hresult_error)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: Failed to register event handlers");
  }
}

#pragma warning(pop)

void DeInit()
{
  s_change_queue.Shutdown();

  WGI::RawGameController::RawGameControllerAdded(s_added_token);
  WGI::RawGameController::RawGameControllerRemoved(s_removed_token);

  if (s_owns_winrt)
  {
    winrt::uninit_apartment();
    s_owns_winrt = false;
  }
}

void PopulateDevices()
{
  // RawGameController exists from Windows 10 1703 (DisplayName and SimpleHapticsControllers
  // from 1709). Not handled specially: arcade sticks, flight sticks, racing wheels and UI
  // navigation controllers.
  try
  {
    for (const WGI::RawGameController& controller : WGI::RawGameController::RawGameControllers())
    {
      RemoveController(controller);
      AddController(controller);
    }
  }
  catch (winrt::hresult_error)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "WGInput: PopulateDevices failed");
  }
}
}  // namespace ciface::WGInput
