// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/Turntable.h"

#include <array>

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/Slider.h"

namespace WiimoteEmu
{
namespace
{
constexpr std::array<u8, 6> TURNTABLE_ID = {0x03, 0x00, 0xa4, 0x20, 0x01, 0x03};

// Report bits of the Buttons group entries, in group order: the six platter buttons, "-",
// "+" and Euphoria.
constexpr std::array<u16, 9> BUTTON_BITS = {
    Turntable::BUTTON_L_GREEN, Turntable::BUTTON_L_RED, Turntable::BUTTON_L_BLUE,
    Turntable::BUTTON_R_GREEN, Turntable::BUTTON_R_RED, Turntable::BUTTON_R_BLUE,
    Turntable::BUTTON_MINUS,   Turntable::BUTTON_PLUS,  Turntable::BUTTON_EUPHORIA,
};

constexpr u16 ALL_BUTTONS = [] {
  u16 mask = 0;
  for (const u16 bit : BUTTON_BITS)
    mask = static_cast<u16>(mask | bit);
  return mask;
}();

constexpr std::array<const char*, 6> PLATTER_BUTTON_NAMES = {
    _trans("Green Left"),  _trans("Red Left"),  _trans("Blue Left"),
    _trans("Green Right"), _trans("Red Right"), _trans("Blue Right"),
};

// Bits [first, first + count) of a two's complement value.
u8 Bits(int value, int first, int count)
{
  return static_cast<u8>((value >> first) & ((1 << count) - 1));
}
}  // namespace

Turntable::Turntable() : Extension1stParty("Turntable", _trans("DJ Turntable"))
{
  using Translatability = ControllerEmu::Translatability;

  m_buttons = new ControllerEmu::Buttons(_trans("Buttons"));
  groups.emplace_back(m_buttons);
  for (const char* name : PLATTER_BUTTON_NAMES)
    m_buttons->AddInput(Translatability::Translate, name);
  m_buttons->AddInput(Translatability::DoNotTranslate, "-");
  m_buttons->AddInput(Translatability::DoNotTranslate, "+");
  // i18n: This button name refers to a gameplay element in DJ Hero
  m_buttons->AddInput(Translatability::Translate, _trans("Euphoria"));

  // i18n: "Table" refers to a turntable
  m_left_table = new ControllerEmu::Slider("Table Left", _trans("Left Table"));
  groups.emplace_back(m_left_table);
  // i18n: "Table" refers to a turntable
  m_right_table = new ControllerEmu::Slider("Table Right", _trans("Right Table"));
  groups.emplace_back(m_right_table);

  m_stick = new ControllerEmu::OctagonAnalogStick(_trans("Stick"),
                                                  ControlState(STICK_GATE_RADIUS) / STICK_RADIUS);
  groups.emplace_back(m_stick);

  m_effect_dial = new ControllerEmu::Slider(_trans("Effect"));
  groups.emplace_back(m_effect_dial);

  m_crossfade = new ControllerEmu::Slider(_trans("Crossfade"));
  groups.emplace_back(m_crossfade);
}

void Turntable::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  using ControllerEmu::MapFloat;

  DataFormat report = {};

  const auto stick = m_stick->GetState(m_input_override_function);
  report.sx = Bits(MapFloat<u8>(stick.x, STICK_CENTER, 0, STICK_RANGE), 0, 6);
  report.sy = Bits(MapFloat<u8>(stick.y, STICK_CENTER, 0, STICK_RANGE), 0, 6);

  // Platter speeds: signed, -31 to +31.
  constexpr s8 SPEED_LIMIT = TABLE_RANGE;
  const auto platter_speed = [&](const ControllerEmu::Slider* platter) -> int {
    return MapFloat<s8>(platter->GetState(m_input_override_function).value, 0, -SPEED_LIMIT,
                        SPEED_LIMIT);
  };

  const int left_speed = platter_speed(m_left_table);
  report.ltable1 = Bits(left_speed, 0, 5);
  report.ltable2 = Bits(left_speed, 5, 1);

  const int right_speed = platter_speed(m_right_table);
  report.rtable1 = Bits(right_speed, 0, 1);
  report.rtable2 = Bits(right_speed, 1, 2);
  report.rtable3 = Bits(right_speed, 3, 2);
  report.rtable4 = Bits(right_speed, 5, 1);

  const u8 dial = MapFloat<u8>(m_effect_dial->GetState(m_input_override_function).value,
                               EFFECT_DIAL_CENTER, 0, EFFECT_DIAL_RANGE);
  report.dial1 = Bits(dial, 0, 3);
  report.dial2 = Bits(dial, 3, 2);

  report.slider = Bits(MapFloat<u8>(m_crossfade->GetState(m_input_override_function).value,
                                    CROSSFADE_CENTER, 0, CROSSFADE_RANGE),
                       0, 4);

  // Buttons are ORed in after ltable2, which shares bit 0 with them (and is not inverted).
  m_buttons->GetState(&report.bt, BUTTON_BITS.data(), m_input_override_function);
  report.bt = static_cast<u16>(report.bt ^ ALL_BUTTONS);

  target_state->data = report;
}

void Turntable::Update(const DesiredExtensionState& target_state)
{
  DefaultExtensionUpdate<DataFormat>(&m_reg, target_state);
}

void Turntable::Reset()
{
  EncryptedExtension::Reset();

  // No calibration data is known for the turntable; the block stays zero.
  m_reg.identifier = TURNTABLE_ID;
}

ControllerEmu::ControlGroup* Turntable::GetGroup(TurntableGroup group)
{
  return GroupByEnum(group, m_buttons, m_stick, m_effect_dial, m_left_table, m_right_table,
                     m_crossfade);
}
}  // namespace WiimoteEmu
