// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/WiimoteEmu/Extension/Extension.h"

namespace ControllerEmu
{
class AnalogStick;
class Buttons;
class ControlGroup;
class Slider;
class Triggers;
}  // namespace ControllerEmu

// The DJ Hero turntable: two platters with three buttons each, a crossfader, an effect dial, a
// stick and the Euphoria button. A first-party scheme extension.

namespace WiimoteEmu
{
enum class TurntableGroup
{
  Buttons,
  Stick,
  EffectDial,
  LeftTable,
  RightTable,
  Crossfade
};

class Turntable : public Extension1stParty
{
public:
  // The six byte report. The platter speeds are signed 6 bit values; the right one is spread
  // over four fields (bit 0, bits 1-2, bits 3-4, bit 5) and the left one over two (bits 0-4 and
  // bit 5, which shares a bit with the buttons). The dial is split into bits 0-2 and 3-4.
  struct DataFormat
  {
    u8 sx : 6;
    u8 rtable3 : 2;

    u8 sy : 6;
    u8 rtable2 : 2;

    u8 rtable4 : 1;
    u8 slider : 4;
    u8 dial2 : 2;
    u8 rtable1 : 1;

    u8 ltable1 : 5;
    u8 dial1 : 3;

    union
    {
      u16 ltable2 : 1;
      // Active-low buttons.
      u16 bt;
    };
  };
  static_assert(sizeof(DataFormat) == 6, "Wrong size");

  using DesiredState = DataFormat;

  Turntable();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;

  ControllerEmu::ControlGroup* GetGroup(TurntableGroup group);

  static constexpr u16 BUTTON_EUPHORIA = 0x1000;

  static constexpr u16 BUTTON_L_GREEN = 0x0800;
  static constexpr u16 BUTTON_L_RED = 0x20;
  static constexpr u16 BUTTON_L_BLUE = 0x8000;

  static constexpr u16 BUTTON_R_GREEN = 0x2000;
  static constexpr u16 BUTTON_R_RED = 0x02;
  static constexpr u16 BUTTON_R_BLUE = 0x0400;

  static constexpr u16 BUTTON_MINUS = 0x10;
  static constexpr u16 BUTTON_PLUS = 0x04;

  static constexpr int STICK_BIT_COUNT = 6;
  static constexpr u8 STICK_CENTER = (1 << STICK_BIT_COUNT) / 2;
  static constexpr u8 STICK_RADIUS = STICK_CENTER - 1;
  static constexpr u8 STICK_RANGE = (1 << STICK_BIT_COUNT) - 1;
  // An estimate; not measured on hardware.
  static constexpr u8 STICK_GATE_RADIUS = 0x16;

  static constexpr int TABLE_BIT_COUNT = 6;
  static constexpr u8 TABLE_RANGE = (1 << TABLE_BIT_COUNT) / 2 - 1;

  static constexpr int EFFECT_DIAL_BIT_COUNT = 5;
  static constexpr u8 EFFECT_DIAL_CENTER = (1 << EFFECT_DIAL_BIT_COUNT) / 2;
  static constexpr u8 EFFECT_DIAL_RANGE = (1 << EFFECT_DIAL_BIT_COUNT) - 1;

  static constexpr int CROSSFADE_BIT_COUNT = 4;
  static constexpr u8 CROSSFADE_CENTER = (1 << CROSSFADE_BIT_COUNT) / 2;
  static constexpr u8 CROSSFADE_RANGE = (1 << CROSSFADE_BIT_COUNT) - 1;

private:
  ControllerEmu::Buttons* m_buttons;
  ControllerEmu::AnalogStick* m_stick;
  ControllerEmu::Slider* m_effect_dial;
  ControllerEmu::Slider* m_left_table;
  ControllerEmu::Slider* m_right_table;
  ControllerEmu::Slider* m_crossfade;
};
}  // namespace WiimoteEmu
