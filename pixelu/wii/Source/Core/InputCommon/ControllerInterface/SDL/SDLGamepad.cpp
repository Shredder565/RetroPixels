// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/SDL/SDLGamepad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string_view>

#include <fmt/format.h>

#include "Common/Logging/Log.h"

namespace ciface::SDL
{
namespace
{
// Gamepad button names, indexed by SDL_GamepadButton. The face buttons are named by position.
constexpr std::array<const char*, 26> BUTTON_NAMES = {
    "Button S", "Button E", "Button W",   "Button N",   "Back",     "Guide",    "Start",
    "Thumb L",  "Thumb R",  "Shoulder L", "Shoulder R", "Pad N",    "Pad S",    "Pad W",
    "Pad E",    "Misc 1",   "Paddle 1",   "Paddle 2",   "Paddle 3", "Paddle 4", "Touchpad",
    "Misc 2",   "Misc 3",   "Misc 4",     "Misc 5",     "Misc 6",
};

// Gamepad axis names, indexed by SDL_GamepadAxis: four stick axes, then the two triggers.
constexpr std::array<const char*, 6> AXIS_NAMES = {
    "Left X", "Left Y", "Right X", "Right Y", "Trigger L", "Trigger R",
};
constexpr int FIRST_TRIGGER_AXIS = 4;

// XInput's names for the face buttons, matched by position so mappings carry across pads.
constexpr std::array<std::pair<SDL_GamepadButton, std::string_view>, 4> XINPUT_FACE_NAMES = {{
    {SDL_GAMEPAD_BUTTON_SOUTH, "Button A"},
    {SDL_GAMEPAD_BUTTON_EAST, "Button B"},
    {SDL_GAMEPAD_BUTTON_WEST, "Button X"},
    {SDL_GAMEPAD_BUTTON_NORTH, "Button Y"},
}};

// One direction of a motion sensor's axis: sensor data component `component` times `sign`.
struct SensorDirection
{
  const char* name;
  int component;
  ControlState sign;
};
using SensorDirections = std::array<SensorDirection, 6>;

constexpr SensorDirections ACCEL_DIRECTIONS = {{
    {"Up", 1, 1},
    {"Down", 1, -1},
    {"Left", 0, -1},
    {"Right", 0, 1},
    {"Forward", 2, -1},
    {"Backward", 2, 1},
}};

constexpr SensorDirections GYRO_DIRECTIONS = {{
    {"Pitch Up", 0, 1},
    {"Pitch Down", 0, -1},
    {"Roll Left", 2, 1},
    {"Roll Right", 2, -1},
    {"Yaw Left", 1, 1},
    {"Yaw Right", 1, -1},
}};

std::string NumberedButtonName(int index)
{
  return fmt::format("Button {}", index);
}

std::string NumberedAxisName(int index, int scale)
{
  return fmt::format("Axis {}{}", index, scale < 0 ? '-' : '+');
}

std::string NumberedHatName(int index, int direction)
{
  return fmt::format("Hat {} {}", index, "NESW"[direction]);
}

bool IsTrigger(int axis)
{
  return axis >= FIRST_TRIGGER_AXIS;
}

// A rumble level as SDL takes it: 0..65535.
long ToRumbleLevel(ControlState state)
{
  return std::lround(state * std::numeric_limits<Uint16>::max());
}

class MappedButton final : public Core::Device::Input
{
public:
  MappedButton(SDL_Gamepad* gamepad, SDL_GamepadButton button)
      : m_gamepad(gamepad), m_button(button)
  {
  }

  std::string GetName() const override
  {
    if (static_cast<size_t>(m_button) < BUTTON_NAMES.size())
      return BUTTON_NAMES[static_cast<size_t>(m_button)];
    return NumberedButtonName(m_button);
  }

  bool IsMatchingName(std::string_view name) const override
  {
    if (name == GetName())
      return true;

    for (const auto& [button, xinput_name] : XINPUT_FACE_NAMES)
    {
      if (m_button == button && name == xinput_name)
        return true;
    }

    // Older configurations used the numbered names.
    return name == NumberedButtonName(m_button);
  }

  ControlState GetState() const override { return SDL_GetGamepadButton(m_gamepad, m_button); }

private:
  SDL_Gamepad* const m_gamepad;
  const SDL_GamepadButton m_button;
};

class MappedAxis final : public Core::Device::Input
{
public:
  MappedAxis(SDL_Gamepad* gamepad, Sint16 scale, SDL_GamepadAxis axis)
      : m_gamepad(gamepad), m_scale(scale), m_axis(axis)
  {
  }

  std::string GetName() const override
  {
    if (static_cast<size_t>(m_axis) >= AXIS_NAMES.size())
      return NumberedAxisName(m_axis, m_scale);

    const std::string base = AXIS_NAMES[static_cast<size_t>(m_axis)];
    if (IsTrigger(m_axis))
      return base;

    // SDL's vertical axes point down; the names follow XInput, where they point up.
    const bool points_down = m_axis % 2 == 1;
    const bool negative = (m_scale < 0) != points_down;
    return base + (negative ? '-' : '+');
  }

  ControlState GetState() const override
  {
    return ControlState(SDL_GetGamepadAxis(m_gamepad, m_axis)) / m_scale;
  }

private:
  SDL_Gamepad* const m_gamepad;
  const Sint16 m_scale;
  const SDL_GamepadAxis m_axis;
};

class RawButton final : public Core::Device::Input
{
public:
  RawButton(SDL_Joystick* joystick, int index) : m_joystick(joystick), m_index(index) {}

  std::string GetName() const override { return NumberedButtonName(m_index); }
  ControlState GetState() const override { return SDL_GetJoystickButton(m_joystick, m_index); }

private:
  SDL_Joystick* const m_joystick;
  const int m_index;
};

// A raw axis direction. Axes the gamepad mapping covers stay available (hidden) so existing
// "Full Axis" mappings keep working.
class RawAxis final : public Core::Device::Input
{
public:
  RawAxis(SDL_Joystick* joystick, int index, s16 scale, bool covered)
      : m_joystick(joystick), m_index(index), m_scale(scale), m_covered(covered)
  {
  }

  std::string GetName() const override { return NumberedAxisName(m_index, m_scale); }
  bool IsHidden() const override { return m_covered; }
  bool IsDetectable() const override { return !IsHidden(); }

  ControlState GetState() const override
  {
    return ControlState(SDL_GetJoystickAxis(m_joystick, m_index)) / m_scale;
  }

private:
  SDL_Joystick* const m_joystick;
  const int m_index;
  const s16 m_scale;
  const bool m_covered;
};

class RawHat final : public Core::Device::Input
{
public:
  // `direction`: 0 north, 1 east, 2 south, 3 west (the bit order of SDL's hat masks).
  RawHat(SDL_Joystick* joystick, int index, u8 direction)
      : m_joystick(joystick), m_index(index), m_direction(direction)
  {
  }

  std::string GetName() const override { return NumberedHatName(m_index, m_direction); }

  ControlState GetState() const override
  {
    return (SDL_GetJoystickHat(m_joystick, m_index) & (1 << m_direction)) > 0;
  }

private:
  SDL_Joystick* const m_joystick;
  const int m_index;
  const u8 m_direction;
};

class BatteryInput final : public Core::Device::Input
{
public:
  explicit BatteryInput(const ControlState* level) : m_level(*level) {}

  std::string GetName() const override { return "Battery"; }
  ControlState GetState() const override { return m_level; }
  bool IsDetectable() const override { return false; }

private:
  const ControlState& m_level;
};

// A touchpad coordinate, signed per direction.
template <int Sign>
class TouchCoordinate final : public Core::Device::Input
{
public:
  TouchCoordinate(std::string name, const float* value) : m_name(std::move(name)), m_value(*value)
  {
  }

  std::string GetName() const override { return m_name + (Sign > 0 ? '+' : '-'); }
  bool IsDetectable() const override { return false; }
  ControlState GetState() const override { return m_value * Sign; }

private:
  const std::string m_name;
  const float& m_value;
};

class TouchPressure final : public Core::Device::Input
{
public:
  TouchPressure(std::string name, const float* value) : m_name(std::move(name)), m_value(*value) {}

  std::string GetName() const override { return m_name; }
  ControlState GetState() const override { return m_value; }

private:
  const std::string m_name;
  const float& m_value;
};

class SensorInput final : public Core::Device::Input
{
public:
  SensorInput(std::string name, SDL_Gamepad* gamepad, SDL_SensorType sensor, int component,
              ControlState sign)
      : m_name(std::move(name)), m_gamepad(gamepad), m_sensor(sensor), m_component(component),
        m_sign(sign)
  {
  }

  std::string GetName() const override { return m_name; }
  bool IsDetectable() const override { return false; }

  ControlState GetState() const override
  {
    std::array<float, 3> reading{};
    SDL_GetGamepadSensorData(m_gamepad, m_sensor, reading.data(), int(reading.size()));
    return m_sign * reading[static_cast<size_t>(m_component)];
  }

private:
  const std::string m_name;
  SDL_Gamepad* const m_gamepad;
  const SDL_SensorType m_sensor;
  const int m_component;
  const ControlState m_sign;
};

// One rumble motor (or one trigger motor).
class RumbleMotor final : public Core::Device::Output
{
public:
  using Sender = void (Gamepad::*)();

  RumbleMotor(const char* name, Gamepad* owner, Uint16* level, Sender send)
      : m_name(name), m_owner(*owner), m_level(*level), m_send(send)
  {
  }

  std::string GetName() const override { return m_name; }

  void SetState(ControlState state) override
  {
    const long wanted = ToRumbleLevel(state);
    if (wanted == m_level)
      return;
    m_level = static_cast<Uint16>(wanted);
    (m_owner.*m_send)();
  }

private:
  const char* const m_name;
  Gamepad& m_owner;
  Uint16& m_level;
  const Sender m_send;
};

// Both rumble motors at once.
class BothMotors final : public Core::Device::Output
{
public:
  BothMotors(Gamepad* owner, Uint16* low, Uint16* high)
      : m_owner(*owner), m_low(*low), m_high(*high)
  {
  }

  std::string GetName() const override { return "Motor"; }

  void SetState(ControlState state) override
  {
    const long wanted = ToRumbleLevel(state);
    if (wanted == m_low && wanted == m_high)
      return;
    m_low = static_cast<Uint16>(wanted);
    m_high = static_cast<Uint16>(wanted);
    m_owner.SendRumble();
  }

private:
  Gamepad& m_owner;
  Uint16& m_low;
  Uint16& m_high;
};

// A haptic effect driven by the output level. The effect is uploaded and started when the level
// becomes non-zero, updated while it stays non-zero, and stopped and destroyed at zero.
class HapticOutput : public Core::Device::Output
{
public:
  explicit HapticOutput(SDL_Haptic* haptic) : m_haptic(haptic) { m_effect.type = NO_EFFECT; }

  ~HapticOutput() override
  {
    m_effect.type = NO_EFFECT;
    Apply();
  }

  void SetState(ControlState state) final
  {
    // SDL's largest effect strength.
    constexpr s16 FULL_STRENGTH = 0x7fff;
    if (SetLevel(static_cast<s16>(state * FULL_STRENGTH)))
      Apply();
  }

protected:
  // A zero effect type marks "nothing to play".
  static constexpr Uint16 NO_EFFECT = 0;

  // Stores the new level in the effect; returns whether it changed.
  virtual bool SetLevel(s16 level) = 0;

  // Pushes towards the left, which is what wheels expect; other devices ignore the direction.
  static void AimLeft(SDL_HapticDirection* direction)
  {
    direction->type = SDL_HAPTIC_CARTESIAN;
    direction->dir[0] = -1;
  }

  SDL_HapticEffect m_effect = {};

private:
  void Apply()
  {
    if (m_effect.type == NO_EFFECT)
    {
      if (m_id < 0)
        return;
      SDL_StopHapticEffect(m_haptic, m_id);
      SDL_DestroyHapticEffect(m_haptic, m_id);
      m_id = -1;
      return;
    }

    if (m_id >= 0)
    {
      SDL_UpdateHapticEffect(m_haptic, m_id, &m_effect);
      return;
    }

    m_id = SDL_CreateHapticEffect(m_haptic, &m_effect);
    if (m_id >= 0)
      SDL_RunHapticEffect(m_haptic, m_id, 1);
  }

  SDL_Haptic* const m_haptic;
  int m_id = -1;
};

class ConstantForce final : public HapticOutput
{
public:
  explicit ConstantForce(SDL_Haptic* haptic) : HapticOutput(haptic)
  {
    m_effect.constant = {};
    AimLeft(&m_effect.constant.direction);
    m_effect.constant.length = RUMBLE_LENGTH_MS;
  }

  std::string GetName() const override { return "Constant"; }

private:
  bool SetLevel(s16 level) override
  {
    const s16 before = m_effect.constant.level;
    m_effect.constant.level = level;
    m_effect.type = m_effect.constant.level ? SDL_HAPTIC_CONSTANT : NO_EFFECT;
    return m_effect.constant.level != before;
  }
};

class RampForce final : public HapticOutput
{
public:
  explicit RampForce(SDL_Haptic* haptic) : HapticOutput(haptic)
  {
    m_effect.ramp = {};
    AimLeft(&m_effect.ramp.direction);
    m_effect.ramp.length = RUMBLE_LENGTH_MS;
  }

  std::string GetName() const override { return "Ramp"; }

private:
  bool SetLevel(s16 level) override
  {
    // A ramp that starts and ends at the same level: rumble has no use for an actual ramp.
    const s16 before = m_effect.ramp.start;
    m_effect.ramp.start = level;
    m_effect.ramp.end = m_effect.ramp.start;
    m_effect.type = m_effect.ramp.start ? SDL_HAPTIC_RAMP : NO_EFFECT;
    return m_effect.ramp.start != before;
  }
};

class PeriodicForce final : public HapticOutput
{
public:
  PeriodicForce(SDL_Haptic* haptic, Uint16 waveform) : HapticOutput(haptic), m_waveform(waveform)
  {
    m_effect.periodic = {};
    AimLeft(&m_effect.periodic.direction);
    m_effect.periodic.length = RUMBLE_LENGTH_MS;
    m_effect.periodic.period = RUMBLE_PERIOD_MS;
    m_effect.periodic.offset = 0;
    m_effect.periodic.phase = 0;
  }

  std::string GetName() const override
  {
    switch (m_waveform)
    {
    case SDL_HAPTIC_SINE:
      return "Sine";
    case SDL_HAPTIC_TRIANGLE:
      return "Triangle";
    case SDL_HAPTIC_SAWTOOTHUP:
      return "Sawtooth Up";
    case SDL_HAPTIC_SAWTOOTHDOWN:
      return "Sawtooth Down";
    default:
      return "Unknown";
    }
  }

private:
  bool SetLevel(s16 level) override
  {
    const s16 before = m_effect.periodic.magnitude;
    m_effect.periodic.magnitude = level;
    m_effect.type = m_effect.periodic.magnitude ? m_waveform : NO_EFFECT;
    return m_effect.periodic.magnitude != before;
  }

  const Uint16 m_waveform;
};

// One motor of a "left/right" (strong and weak motor) effect.
class DualMotorForce final : public HapticOutput
{
public:
  DualMotorForce(SDL_Haptic* haptic, bool strong) : HapticOutput(haptic), m_strong(strong)
  {
    m_effect.leftright = {};
    m_effect.leftright.length = RUMBLE_LENGTH_MS;
  }

  std::string GetName() const override { return m_strong ? "Strong" : "Weak"; }

private:
  bool SetLevel(s16 level) override
  {
    Uint16& magnitude =
        m_strong ? m_effect.leftright.large_magnitude : m_effect.leftright.small_magnitude;
    const Uint16 before = magnitude;
    magnitude = static_cast<Uint16>(level);
    m_effect.type = magnitude ? SDL_HAPTIC_LEFTRIGHT : NO_EFFECT;
    return magnitude != before;
  }

  const bool m_strong;
};

bool Contains(const std::vector<int>& list, int value)
{
  return std::ranges::find(list, value) != list.end();
}
}  // namespace

struct Gamepad::CoveredControls
{
  std::vector<int> buttons;
  std::vector<int> axes;
  std::vector<int> hats;

  void Add(const SDL_GamepadBinding& binding)
  {
    switch (binding.input_type)
    {
    case SDL_GAMEPAD_BINDTYPE_BUTTON:
      buttons.push_back(binding.input.button);
      break;
    case SDL_GAMEPAD_BINDTYPE_AXIS:
      axes.push_back(binding.input.axis.axis);
      break;
    case SDL_GAMEPAD_BINDTYPE_HAT:
      hats.push_back(binding.input.hat.hat);
      break;
    default:
      break;
    }
  }
};

Gamepad::Gamepad(SDL_Gamepad* const gamepad, SDL_Joystick* const joystick)
    : m_gamepad(gamepad), m_joystick(joystick)
{
  const char* const sdl_name =
      gamepad != nullptr ? SDL_GetGamepadName(gamepad) : SDL_GetJoystickName(joystick);
  m_name = sdl_name != nullptr ? sdl_name : "Unknown";

  CoveredControls covered;
  if (m_gamepad != nullptr)
    AddMappedControls(&covered);

  AddRawControls(covered);
  AddHaptics();

  if (ReadBattery())
    AddInput(new BatteryInput(&m_battery));
}

void Gamepad::AddMappedControls(CoveredControls* covered)
{
  int binding_count = 0;
  SDL_GamepadBinding** const bindings = SDL_GetGamepadBindings(m_gamepad, &binding_count);

  // Controls the mapping provides. Their raw joystick sources are noted so the raw buttons and
  // hats are not listed twice.
  for (const SDL_GamepadBinding* binding : std::span(bindings, static_cast<size_t>(binding_count)))
  {
    covered->Add(*binding);

    if (binding->output_type == SDL_GAMEPAD_BINDTYPE_BUTTON)
    {
      AddInput(new MappedButton(m_gamepad, binding->output.button));
    }
    else if (binding->output_type == SDL_GAMEPAD_BINDTYPE_AXIS)
    {
      const SDL_GamepadAxis axis = binding->output.axis.axis;
      // Triggers rest at zero and only go one way.
      if (!IsTrigger(axis))
        AddInput(new MappedAxis(m_gamepad, -32768, axis));
      AddInput(new MappedAxis(m_gamepad, 32767, axis));
    }
  }

  const SDL_PropertiesID properties = SDL_GetGamepadProperties(m_gamepad);

  if (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false))
  {
    AddOutput(new BothMotors(this, &m_rumble.low_frequency, &m_rumble.high_frequency));
    AddOutput(new RumbleMotor("Motor L", this, &m_rumble.low_frequency, &Gamepad::SendRumble));
    AddOutput(new RumbleMotor("Motor R", this, &m_rumble.high_frequency, &Gamepad::SendRumble));
  }
  if (SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false))
  {
    AddOutput(
        new RumbleMotor("Trigger L", this, &m_rumble.left_trigger, &Gamepad::SendTriggerRumble));
    AddOutput(
        new RumbleMotor("Trigger R", this, &m_rumble.right_trigger, &Gamepad::SendTriggerRumble));
  }

  const int touchpad_count = SDL_GetNumGamepadTouchpads(m_gamepad);
  if (touchpad_count > 0)
  {
    m_touchpads.resize(static_cast<size_t>(touchpad_count));
    for (int pad = 0; pad != touchpad_count; ++pad)
    {
      // The first touchpad is unnumbered, as configurations expect "Touchpad X".
      const std::string prefix = pad == 0 ? "Touchpad " : fmt::format("Touchpad {} ", pad);
      Touchpad& state = m_touchpads[static_cast<size_t>(pad)];
      AddInput(new TouchCoordinate<-1>(prefix + "X", &state.x));
      AddInput(new TouchCoordinate<+1>(prefix + "X", &state.x));
      AddInput(new TouchCoordinate<-1>(prefix + "Y", &state.y));
      AddInput(new TouchCoordinate<+1>(prefix + "Y", &state.y));
      AddInput(new TouchPressure(prefix + "Pressure", &state.pressure));
    }
  }

  struct Sensor
  {
    SDL_SensorType type;
    const char* name;
    const SensorDirections* directions;
  };
  static constexpr Sensor sensors[] = {
      {SDL_SENSOR_ACCEL, "Accel", &ACCEL_DIRECTIONS},
      {SDL_SENSOR_GYRO, "Gyro", &GYRO_DIRECTIONS},
      {SDL_SENSOR_ACCEL_L, "Accel L", &ACCEL_DIRECTIONS},
      {SDL_SENSOR_GYRO_L, "Gyro L", &GYRO_DIRECTIONS},
      {SDL_SENSOR_ACCEL_R, "Accel R", &ACCEL_DIRECTIONS},
      {SDL_SENSOR_GYRO_R, "Gyro R", &GYRO_DIRECTIONS},
  };
  for (const Sensor& sensor : sensors)
  {
    if (!SDL_SetGamepadSensorEnabled(m_gamepad, sensor.type, true))
      continue;
    for (const SensorDirection& direction : *sensor.directions)
    {
      AddInput(new SensorInput(fmt::format("{} {}", sensor.name, direction.name), m_gamepad,
                               sensor.type, direction.component, direction.sign));
    }
  }

  SDL_free(bindings);
}

void Gamepad::AddRawControls(const CoveredControls& covered)
{
  int button_count = SDL_GetNumJoystickButtons(m_joystick);
  if (button_count < 0)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "Error in SDL_JoystickNumButtons(): {}", SDL_GetError());
    button_count = 0;
  }
  for (int button = 0; button != button_count; ++button)
  {
    if (!Contains(covered.buttons, button))
      AddInput(new RawButton(m_joystick, button));
  }

  int axis_count = SDL_GetNumJoystickAxes(m_joystick);
  if (axis_count < 0)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "Error in SDL_JoystickNumAxes(): {}", SDL_GetError());
    axis_count = 0;
  }
  for (int axis = 0; axis != axis_count; ++axis)
  {
    const bool is_covered = Contains(covered.axes, axis);
    AddFullAnalogSurfaceInputs(new RawAxis(m_joystick, axis, -32768, is_covered),
                               new RawAxis(m_joystick, axis, 32767, is_covered));
  }

  int hat_count = SDL_GetNumJoystickHats(m_joystick);
  if (hat_count < 0)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "Error in SDL_JoystickNumHats(): {}", SDL_GetError());
    hat_count = 0;
  }
  for (int hat = 0; hat != hat_count; ++hat)
  {
    if (Contains(covered.hats, hat))
      continue;
    for (u8 direction = 0; direction != 4; ++direction)
      AddInput(new RawHat(m_joystick, hat, direction));
  }
}

void Gamepad::AddHaptics()
{
  if (!SDL_IsJoystickHaptic(m_joystick))
    return;

  m_haptic = SDL_OpenHapticFromJoystick(m_joystick);
  if (m_haptic == nullptr)
    return;

  // The effect bits are tested against SDL_GetMaxHapticEffects() (as PixelU-Wii always did).
  const unsigned int effects = static_cast<unsigned int>(SDL_GetMaxHapticEffects(m_haptic));

  if (effects & SDL_HAPTIC_AUTOCENTER)
    SDL_SetHapticAutocenter(m_haptic, 0);

  if (effects & SDL_HAPTIC_CONSTANT)
    AddOutput(new ConstantForce(m_haptic));
  if (effects & SDL_HAPTIC_RAMP)
    AddOutput(new RampForce(m_haptic));

  for (const unsigned int waveform :
       {SDL_HAPTIC_SINE, SDL_HAPTIC_TRIANGLE, SDL_HAPTIC_SAWTOOTHUP, SDL_HAPTIC_SAWTOOTHDOWN})
  {
    if (effects & waveform)
      AddOutput(new PeriodicForce(m_haptic, static_cast<Uint16>(waveform)));
  }

  if (effects & SDL_HAPTIC_LEFTRIGHT)
  {
    AddOutput(new DualMotorForce(m_haptic, true));
    AddOutput(new DualMotorForce(m_haptic, false));
  }
}

bool Gamepad::ReadBattery()
{
  int percent = 0;
  if (SDL_GetJoystickPowerInfo(m_joystick, &percent) == SDL_POWERSTATE_ERROR)
    return false;

  m_battery = std::max(0, percent);
  return true;
}

std::string Gamepad::GetName() const
{
  return m_name;
}

std::string Gamepad::GetSource() const
{
  return "SDL";
}

SDL_JoystickID Gamepad::GetSDLInstanceID() const
{
  return SDL_GetJoystickID(m_joystick);
}

Gamepad::~Gamepad()
{
  if (m_haptic != nullptr)
  {
    // Stops every effect; the haptic is closed before the joystick it belongs to.
    SDL_StopHapticEffects(m_haptic);
    SDL_CloseHaptic(m_haptic);
    m_haptic = nullptr;
  }
  if (m_gamepad != nullptr)
  {
    SDL_RumbleGamepad(m_gamepad, 0, 0, 0);
    SDL_CloseGamepad(m_gamepad);
  }
  SDL_CloseJoystick(m_joystick);
}

Core::DeviceRemoval Gamepad::UpdateInput()
{
  ReadBattery();

  // Only the first finger on each touchpad is followed.
  constexpr int FINGER = 0;
  for (int pad = 0; pad != static_cast<int>(m_touchpads.size()); ++pad)
  {
    if (SDL_GetNumGamepadTouchpadFingers(m_gamepad, pad) <= FINGER)
      continue;

    Touchpad& state = m_touchpads[static_cast<size_t>(pad)];
    SDL_GetGamepadTouchpadFinger(m_gamepad, pad, FINGER, nullptr, &state.x, &state.y,
                                 &state.pressure);
    // 0..1 from SDL, -1..1 for us.
    state.x = state.x * 2 - 1;
    state.y = state.y * 2 - 1;
  }

  return Core::DeviceRemoval::Keep;
}

void Gamepad::SendRumble()
{
  SDL_RumbleGamepad(m_gamepad, m_rumble.low_frequency, m_rumble.high_frequency, RUMBLE_LENGTH_MS);
}

void Gamepad::SendTriggerRumble()
{
  SDL_RumbleGamepadTriggers(m_gamepad, m_rumble.left_trigger, m_rumble.right_trigger,
                            RUMBLE_LENGTH_MS);
}

}  // namespace ciface::SDL
