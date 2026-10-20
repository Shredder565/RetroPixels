// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GCPadEmu.h"

#include <array>

#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/GCPad.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/MixedTriggers.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/GCPadStatus.h"

namespace
{
// Status bits for each control, in the order the controls are added below.
// The face-button list has a trailing zero for spare slots.
constexpr std::array<u16, 7> FACE_BUTTON_BITS = {
    PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y, PAD_TRIGGER_Z, PAD_BUTTON_START, 0,
};
constexpr std::array<u16, 4> DPAD_BITS = {
    PAD_BUTTON_UP,
    PAD_BUTTON_DOWN,
    PAD_BUTTON_LEFT,
    PAD_BUTTON_RIGHT,
};
constexpr std::array<u16, 2> TRIGGER_BITS = {PAD_TRIGGER_L, PAD_TRIGGER_R};
constexpr std::array<u8, 3> CABINET_SWITCH_BITS = {SWITCH_TEST, SWITCH_SERVICE, SWITCH_COIN};

struct DefaultMapping
{
  int index;
  const char* expression;
};

// Default keyboard mapping (Windows key names).
constexpr DefaultMapping FACE_DEFAULTS[] = {
    {0, "`X`"}, {1, "`Z`"}, {2, "`C`"}, {3, "`S`"}, {4, "`D`"}, {5, "`RETURN`"},
};
constexpr DefaultMapping DPAD_DEFAULTS[] = {{0, "`T`"}, {1, "`G`"}, {2, "`F`"}, {3, "`H`"}};
constexpr DefaultMapping CABINET_DEFAULTS[] = {{0, "`1`"}, {1, "`2`"}, {2, "`3`"}};
// Stick controls are up, down, left, right, then the modifier.
constexpr DefaultMapping C_STICK_DEFAULTS[] = {
    {0, "`I`"}, {1, "`K`"}, {2, "`J`"}, {3, "`L`"}, {4, "`Ctrl`"},
};
constexpr DefaultMapping MAIN_STICK_DEFAULTS[] = {
    {0, "`UP`"}, {1, "`DOWN`"}, {2, "`LEFT`"}, {3, "`RIGHT`"}, {4, "`Shift`"},
};
constexpr DefaultMapping TRIGGER_DEFAULTS[] = {{0, "`Q`"}, {1, "`W`"}};

template <std::size_t N>
void ApplyDefaults(ControllerEmu::ControlGroup* group, const DefaultMapping (&mappings)[N])
{
  for (const DefaultMapping& mapping : mappings)
    group->SetControlExpression(mapping.index, mapping.expression);
}
}  // namespace

GCPad::GCPad(const unsigned int index) : m_index(index)
{
  using ControllerEmu::Translatability;

  m_buttons = new ControllerEmu::Buttons(BUTTONS_GROUP);
  groups.emplace_back(m_buttons);
  for (const char* name : {A_BUTTON, B_BUTTON, X_BUTTON, Y_BUTTON, Z_BUTTON})
    m_buttons->AddInput(Translatability::DoNotTranslate, name);
  // i18n: The Start/Pause button of a GameCube controller
  m_buttons->AddInput(Translatability::Translate, START_BUTTON, _trans("START"));

  m_main_stick = new ControllerEmu::OctagonAnalogStick(MAIN_STICK_GROUP, _trans("Control Stick"),
                                                       MAIN_STICK_GATE_RADIUS);
  groups.emplace_back(m_main_stick);
  m_c_stick =
      new ControllerEmu::OctagonAnalogStick(C_STICK_GROUP, _trans("C Stick"), C_STICK_GATE_RADIUS);
  groups.emplace_back(m_c_stick);

  m_triggers = new ControllerEmu::MixedTriggers(TRIGGERS_GROUP);
  groups.emplace_back(m_triggers);
  for (const char* name : {L_DIGITAL, R_DIGITAL, L_ANALOG, R_ANALOG})
    m_triggers->AddInput(Translatability::Translate, name);

  m_dpad = new ControllerEmu::Buttons(DPAD_GROUP);
  groups.emplace_back(m_dpad);
  for (const char* name : named_directions)
    m_dpad->AddInput(Translatability::Translate, name);

  m_triforce = new ControllerEmu::Buttons(TRIFORCE_GROUP);
  groups.emplace_back(m_triforce);
  for (const char* name : {TEST_BUTTON, SERVICE_BUTTON, COIN_BUTTON})
    m_triforce->AddInput(Translatability::Translate, name);

  m_mic = new ControllerEmu::Buttons(MIC_GROUP);
  groups.emplace_back(m_mic);
  m_mic->AddInput(Translatability::Translate, _trans("Button"));

  m_rumble = new ControllerEmu::ControlGroup(RUMBLE_GROUP);
  groups.emplace_back(m_rumble);
  m_rumble->AddOutput(Translatability::Translate, _trans("Motor"));

  m_options = new ControllerEmu::ControlGroup(OPTIONS_GROUP);
  groups.emplace_back(m_options);
  m_options->AddSetting(
      &m_always_connected_setting,
      // i18n: Report the emulated pad as plugged in even when its mapped device is missing
      {_trans("Always Connected"), nullptr,
       _trans("If checked, the emulated controller is always connected.\n"
              "If unchecked, the connection state of the emulated controller is linked\n"
              "to the connection state of the real default device (if there is one).")},
      false);
}

InputConfig* GCPad::GetConfig() const
{
  return Pad::GetConfig();
}

std::string GCPad::GetName() const
{
  // "GCPad1" .. "GCPad4"
  return std::string("GCPad") + static_cast<char>('1' + m_index);
}

ControllerEmu::ControlGroup* GCPad::GetGroup(PadGroup group)
{
  // In PadGroup order.
  ControllerEmu::ControlGroup* const by_id[] = {
      m_buttons, m_main_stick, m_c_stick, m_dpad, m_triggers,
      m_rumble,  m_mic,        m_options, m_triforce,
  };
  const auto id = static_cast<std::size_t>(group);
  return id < std::size(by_id) ? by_id[id] : nullptr;
}

GCPadStatus GCPad::GetInput() const
{
  using ControllerEmu::MapFloat;

  const auto state_lock = GetStateLock();
  GCPadStatus status = {};

  const bool connected = m_always_connected_setting.GetValue() || IsDefaultDeviceConnected() ||
                         m_input_override_function;
  if (!connected)
  {
    status.isConnected = false;
    return status;
  }

  // The controls are read in a fixed order: face buttons, D-pad, cabinet
  // switches, main stick, C-stick, triggers.
  m_buttons->GetState(&status.button, FACE_BUTTON_BITS.data(), m_input_override_function);
  // Analog A/B simply read fully pressed while the button is down.
  if ((status.button & PAD_BUTTON_A) != 0)
    status.analogA = 0xFF;
  if ((status.button & PAD_BUTTON_B) != 0)
    status.analogB = 0xFF;

  m_dpad->GetState(&status.button, DPAD_BITS.data(), m_input_override_function);
  m_triforce->GetState(&status.switches, CABINET_SWITCH_BITS.data(), m_input_override_function);

  const auto main = m_main_stick->GetState(m_input_override_function);
  status.stickX = MapFloat<u8>(main.x, GCPadStatus::MAIN_STICK_CENTER_X, 1);
  status.stickY = MapFloat<u8>(main.y, GCPadStatus::MAIN_STICK_CENTER_Y, 1);

  const auto c = m_c_stick->GetState(m_input_override_function);
  status.substickX = MapFloat<u8>(c.x, GCPadStatus::C_STICK_CENTER_X, 1);
  status.substickY = MapFloat<u8>(c.y, GCPadStatus::C_STICK_CENTER_Y, 1);

  std::array<ControlState, 2> analog{};
  m_triggers->GetState(&status.button, TRIGGER_BITS.data(), analog.data(),
                       m_input_override_function);
  status.triggerLeft = MapFloat<u8>(analog[0], 0);
  status.triggerRight = MapFloat<u8>(analog[1], 0);

  return status;
}

void GCPad::SetOutput(const ControlState strength)
{
  const auto state_lock = GetStateLock();
  m_rumble->controls[0]->control_ref->State(strength);
}

void GCPad::LoadDefaults(const ControllerInterface& ciface)
{
  EmulatedController::LoadDefaults(ciface);

  ApplyDefaults(m_buttons, FACE_DEFAULTS);
  ApplyDefaults(m_dpad, DPAD_DEFAULTS);
  ApplyDefaults(m_triforce, CABINET_DEFAULTS);
  ApplyDefaults(m_c_stick, C_STICK_DEFAULTS);
  ApplyDefaults(m_main_stick, MAIN_STICK_DEFAULTS);

  // Keyboard keys give digital stick input, so calibrate both sticks as squares.
  m_c_stick->SetCalibrationFromGate(ControllerEmu::SquareStickGate(1.0));
  m_main_stick->SetCalibrationFromGate(ControllerEmu::SquareStickGate(1.0));

  ApplyDefaults(m_triggers, TRIGGER_DEFAULTS);
}

bool GCPad::GetMicButton() const
{
  const auto state_lock = GetStateLock();
  return m_mic->controls.back()->GetState<bool>();
}
