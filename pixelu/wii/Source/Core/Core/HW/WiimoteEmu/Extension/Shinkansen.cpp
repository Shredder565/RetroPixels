// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/Shinkansen.h"

#include <array>
#include <variant>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/Triggers.h"

namespace WiimoteEmu
{
namespace
{
// Only the final byte has been confirmed on hardware.
constexpr std::array<u8, 6> SHINKANSEN_ID = {0x00, 0x00, 0xa4, 0x20, 0x01, 0x10};

// The lever notches the game recognizes; anything else counts as fully released (brake 0,
// power 255), which is a guess.
constexpr std::array<u8, 9> BRAKE_NOTCHES = {0, 53, 79, 105, 132, 159, 187, 217, 250};
constexpr std::array<u8, 14> POWER_NOTCHES = {255, 229, 208, 189, 170, 153, 135,
                                              118, 101, 85,  68,  51,  35,  17};

// Report bits of the Buttons group entries (as a little endian u16), in group order:
// Up, Down, Left, Right, A, B, C, D, Select, Start.
constexpr std::array<u16, 10> BUTTON_BITS = {0x0100, 0x0040, 0x0200, 0x0080, 0x2000,
                                             0x4000, 0x1000, 0x0800, 0x0010, 0x0004};

// Picks one of `notches` for a 0..1 lever position.
template <std::size_t N>
u8 Notch(const std::array<u8, N>& notches, ControlState position)
{
  const int index = ControllerEmu::MapFloat(position, 0, 0, static_cast<int>(N - 1));
  return notches[static_cast<std::size_t>(index)];
}
}  // namespace

Shinkansen::Shinkansen() : Extension3rdParty("Shinkansen", _trans("Shinkansen Controller"))
{
  using Translatability = ControllerEmu::Translatability;

  // Physical layout: a d-pad on the left, Select and Start in the middle, and A-D arranged in a
  // diamond on the right (D top, B bottom, A left, C right).
  m_buttons = new ControllerEmu::Buttons(_trans("Buttons"));
  groups.emplace_back(m_buttons);
  for (const char* direction : {_trans("Up"), _trans("Down"), _trans("Left"), _trans("Right")})
    m_buttons->AddInput(Translatability::Translate, direction);
  for (const char* letter : {"A", "B", "C", "D"})
    m_buttons->AddInput(Translatability::DoNotTranslate, letter);
  m_buttons->AddInput(Translatability::Translate, _trans("SELECT"));
  m_buttons->AddInput(Translatability::Translate, _trans("START"));

  // The power lever is mapped inverted so that with no trigger pulled the train coasts.
  m_levers = new ControllerEmu::Triggers(_trans("Levers"));
  groups.emplace_back(m_levers);
  m_levers->AddInput(Translatability::Translate, _trans("L"));
  m_levers->AddInput(Translatability::Translate, _trans("R"));

  // i18n: Noun. This is used as a label for a control group that represents lights on controllers.
  m_led = new ControllerEmu::ControlGroup(_trans("Light"));
  groups.emplace_back(m_led);
  // i18n: A light on the Shinkansen controller that lights up when the train doors are locked.
  // Labeled 戸じめ (in Japanese) on the actual controller.
  m_led->AddOutput(Translatability::Translate, _trans("Doors Locked"));
}

// Note: the levers and buttons are read without the input override function, as they always
// have been.
void Shinkansen::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  DesiredState& desired = target_state->data.emplace<DesiredState>();

  const auto levers = m_levers->GetState().data;
  desired.brake = Notch(BRAKE_NOTCHES, levers[0]);
  desired.power = Notch(POWER_NOTCHES, levers[1]);

  m_buttons->GetState(&desired.buttons, BUTTON_BITS.data());
}

void Shinkansen::Update(const DesiredExtensionState& target_state)
{
  // Without input: brake released, no power, nothing pressed.
  DesiredState desired{0, 255, 0};
  if (const auto* const state = std::get_if<DesiredState>(&target_state.data))
    desired = *state;

  DataFormat report = {};
  report.brake = desired.brake;
  report.power = desired.power;
  report.buttons = static_cast<u16>(~desired.buttons);
  Common::BitCastPtr<DataFormat>(m_reg.controller_data) = report;

  // Drive the light from identifier byte 1, as established.
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  m_led->controls[0]->control_ref->State(m_reg.identifier[1]);
}

void Shinkansen::Reset()
{
  EncryptedExtension::Reset();

  m_reg = {};
  m_reg.identifier = SHINKANSEN_ID;
  m_reg.calibration.fill(0xff);
}

ControllerEmu::ControlGroup* Shinkansen::GetGroup(ShinkansenGroup group)
{
  return GroupByEnum(group, m_buttons, m_levers, m_led);
}
}  // namespace WiimoteEmu
