// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/Classic.h"

#include <array>

#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/MixedTriggers.h"

namespace WiimoteEmu
{
namespace
{
constexpr std::array<u8, 6> CLASSIC_ID = {0x00, 0x00, 0xa4, 0x20, 0x01, 0x01};

// Report bits of the Buttons group entries, in the order they are added below.
constexpr std::array<u16, 9> BUTTON_BITS = {
    Classic::BUTTON_A,     Classic::BUTTON_B,    Classic::BUTTON_X,
    Classic::BUTTON_Y,     Classic::BUTTON_ZL,   Classic::BUTTON_ZR,
    Classic::BUTTON_MINUS, Classic::BUTTON_PLUS, Classic::BUTTON_HOME,
};

// Digital L and R of the Triggers group.
constexpr std::array<u16, 2> TRIGGER_CLICK_BITS = {Classic::TRIGGER_L, Classic::TRIGGER_R};

// Up, down, left, right.
constexpr std::array<u16, 4> DPAD_BITS = {Classic::PAD_UP, Classic::PAD_DOWN, Classic::PAD_LEFT,
                                          Classic::PAD_RIGHT};

// Maps a -1..1 stick axis onto 0..range around center.
u8 StickAxis(ControlState value, u8 center, u8 range)
{
  return ControllerEmu::MapFloat<u8>(value, center, 0, range);
}
}  // namespace

Classic::Classic() : Extension1stParty("Classic", _trans("Classic Controller"))
{
  using Translatability = ControllerEmu::Translatability;

  m_buttons = new ControllerEmu::Buttons(BUTTONS_GROUP);
  groups.emplace_back(m_buttons);
  for (const char* name :
       {A_BUTTON, B_BUTTON, X_BUTTON, Y_BUTTON, ZL_BUTTON, ZR_BUTTON, MINUS_BUTTON, PLUS_BUTTON})
  {
    m_buttons->AddInput(Translatability::DoNotTranslate, name);
  }
  m_buttons->AddInput(Translatability::DoNotTranslate, HOME_BUTTON, "HOME");

  const ControlState gate = ControlState(STICK_GATE_RADIUS) / CAL_STICK_RADIUS;
  m_left_stick = new ControllerEmu::OctagonAnalogStick(LEFT_STICK_GROUP, gate);
  groups.emplace_back(m_left_stick);
  m_right_stick = new ControllerEmu::OctagonAnalogStick(RIGHT_STICK_GROUP, gate);
  groups.emplace_back(m_right_stick);

  m_triggers = new ControllerEmu::MixedTriggers(TRIGGERS_GROUP);
  groups.emplace_back(m_triggers);
  for (const char* name : {L_DIGITAL, R_DIGITAL, L_ANALOG, R_ANALOG})
    m_triggers->AddInput(Translatability::Translate, name);

  m_dpad = new ControllerEmu::Buttons(DPAD_GROUP);
  groups.emplace_back(m_dpad);
  for (const char* direction : named_directions)
    m_dpad->AddInput(Translatability::Translate, direction);
}

void Classic::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  DataFormat report = {};

  const auto left = m_left_stick->GetState(m_input_override_function);
  report.SetLeftStick({StickAxis(left.x, LEFT_STICK_CENTER, LEFT_STICK_RANGE),
                       StickAxis(left.y, LEFT_STICK_CENTER, LEFT_STICK_RANGE)});

  const auto right = m_right_stick->GetState(m_input_override_function);
  report.SetRightStick({StickAxis(right.x, RIGHT_STICK_CENTER, RIGHT_STICK_RANGE),
                        StickAxis(right.y, RIGHT_STICK_CENTER, RIGHT_STICK_RANGE)});

  // The digital trigger clicks, buttons and d-pad all go into one mask.
  u16 pressed = 0;

  ControlState analog[2] = {0, 0};
  m_triggers->GetState(&pressed, TRIGGER_CLICK_BITS.data(), analog, m_input_override_function);
  report.SetLeftTrigger(ControllerEmu::MapFloat<u8>(analog[0], 0, 0, TRIGGER_RANGE));
  report.SetRightTrigger(ControllerEmu::MapFloat<u8>(analog[1], 0, 0, TRIGGER_RANGE));

  m_buttons->GetState(&pressed, BUTTON_BITS.data(), m_input_override_function);
  m_dpad->GetState(&pressed, DPAD_BITS.data(), m_input_override_function);
  report.SetButtons(pressed);

  target_state->data = report;
}

void Classic::Update(const DesiredExtensionState& target_state)
{
  DefaultExtensionUpdate<DataFormat>(&m_reg, target_state);
}

void Classic::Reset()
{
  EncryptedExtension::Reset();

  m_reg.identifier = CLASSIC_ID;

  // Four stick axes (left x, left y, right x, right y) as max/min/center, both trigger
  // neutral points at zero, then the checksum.
  constexpr u8 HIGH = CAL_STICK_CENTER + STICK_GATE_RADIUS;
  constexpr u8 LOW = CAL_STICK_CENTER - STICK_GATE_RADIUS;
  constexpr u8 MID = CAL_STICK_CENTER;
  m_reg.calibration = {HIGH, LOW, MID, HIGH, LOW, MID, HIGH, LOW, MID, HIGH, LOW, MID, 0, 0, 0, 0};

  UpdateCalibrationDataChecksum(m_reg.calibration, CALIBRATION_CHECKSUM_BYTES);
}

ControllerEmu::ControlGroup* Classic::GetGroup(ClassicGroup group)
{
  return GroupByEnum(group, m_buttons, m_triggers, m_dpad, m_left_stick, m_right_stick);
}
}  // namespace WiimoteEmu
