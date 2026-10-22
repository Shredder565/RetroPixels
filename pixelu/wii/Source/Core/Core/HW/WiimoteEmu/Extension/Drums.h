// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/BitField.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
class AnalogStick;
class Buttons;
class ControlGroup;
}  // namespace ControllerEmu

// The Guitar Hero World Tour / Rock Band drum kit: six pads, a stick and two buttons. Each hit
// is reported once with a velocity and the pad's bit is then held for a few reports. A
// first-party scheme extension.

namespace WiimoteEmu
{
enum class DrumsGroup
{
  Buttons,
  Pads,
  Stick
};

class Drums : public Extension1stParty
{
public:
  // The input of one update, before it is turned into reports.
  struct DesiredState
  {
    u8 stick_x;    // 0-63
    u8 stick_y;    // 0-63
    u8 buttons;    // BUTTON_* mask
    u8 drum_pads;  // PAD_* mask of pads held down
    u8 softness;   // 0 (hardest) to 127
  };

  // The six byte report.
  struct DataFormat
  {
    u8 stick_x : 6;
    // These two bits look random on hardware.
    u8 unk1 : 2;

    u8 stick_y : 6;
    u8 unk2 : 2;

    u8 velocity3 : 1;

    // VelocityID of the pad the velocity belongs to.
    u8 velocity_id : 7;

    u8 velocity2 : 1;
    // Both of these read 1 when the report carries no velocity.
    u8 no_velocity_data_1 : 1;
    // Always 0b11 on hardware.
    u8 unk5 : 2;
    u8 no_velocity_data_2 : 1;

    u8 velocity4 : 3;

    // Buttons (active low). Bits 0 and 7 carry velocity bits 0 and 1.
    union
    {
      u8 buttons;
      BitField<0, 1, u8> velocity0;
      BitField<7, 1, u8> velocity1;
    };

    // Pads (active low).
    u8 drum_pads;
  };
  static_assert(sizeof(DataFormat) == 6, "Wrong size");

  enum class VelocityID : u8
  {
    None = 0b1111111,
    Bass = 0b1011011,
    // The hi-hat pedal (0b0011011) is not emulated.
    Red = 0b1011001,
    Yellow = 0b1010001,
    Blue = 0b1001111,
    Orange = 0b1001110,
    Green = 0b1010010,
  };

  Drums();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;

  ControllerEmu::ControlGroup* GetGroup(DrumsGroup group);

  void DoState(PointerWrap& p) override;

  static constexpr u8 BUTTON_PLUS = 0x04;
  static constexpr u8 BUTTON_MINUS = 0x10;

  // Pad bits. The hi-hat pedal has none.
  static constexpr u8 PAD_BASS = 0x04;
  static constexpr u8 PAD_BLUE = 0x08;
  static constexpr u8 PAD_GREEN = 0x10;
  static constexpr u8 PAD_YELLOW = 0x20;
  static constexpr u8 PAD_RED = 0x40;
  static constexpr u8 PAD_ORANGE = 0x80;

  // A real kit's stick covers the whole 0-0x3f range and rests exactly at 0x20.
  static constexpr ControlState GATE_RADIUS = 1.0;
  static constexpr u8 STICK_MIN = 0x00;
  static constexpr u8 STICK_CENTER = 0x20;
  static constexpr u8 STICK_MAX = 0x3f;

private:
  ControllerEmu::Buttons* m_buttons;
  ControllerEmu::Buttons* m_pads;
  ControllerEmu::AnalogStick* m_stick;

  ControllerEmu::SettingValue<double> m_hit_strength_setting;

  // Pads held during the previous update, to spot new hits.
  u8 m_prev_pad_input = 0;
  // Hits whose velocity has not been reported yet.
  u8 m_new_pad_hits = 0;
  // Reports left in which each pad's bit is still set.
  std::array<u8, 6> m_pad_remaining_frames{};
};
}  // namespace WiimoteEmu
