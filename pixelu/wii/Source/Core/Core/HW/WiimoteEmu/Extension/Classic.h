// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <limits>

#include "Common/Matrix.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"

namespace ControllerEmu
{
class AnalogStick;
class Buttons;
class ControlGroup;
class MixedTriggers;
}  // namespace ControllerEmu

// The Classic Controller (and Classic Controller Pro): two sticks, two analog triggers, a d-pad
// and eleven buttons (WiiBrew: "Wiimote/Extension Controllers/Classic Controller").

namespace WiimoteEmu
{
enum class ClassicGroup
{
  Buttons,
  Triggers,
  DPad,
  LeftStick,
  RightStick
};

class Classic : public Extension1stParty
{
public:
  // Bytes 4 and 5 of the report; every button is active low.
  union ButtonFormat
  {
    u16 hex;

    struct
    {
      u8 : 1;
      u8 rt : 1;
      u8 plus : 1;
      u8 home : 1;
      u8 minus : 1;
      u8 lt : 1;
      u8 dpad_down : 1;
      u8 dpad_right : 1;

      u8 dpad_up : 1;
      u8 dpad_left : 1;
      u8 zr : 1;
      u8 x : 1;
      u8 a : 1;
      u8 y : 1;
      u8 b : 1;
      u8 zl : 1;
    };
  };
  static_assert(sizeof(ButtonFormat) == 2, "Wrong size");

  static constexpr int LEFT_STICK_BITS = 6;
  static constexpr int RIGHT_STICK_BITS = 5;
  static constexpr int TRIGGER_BITS = 5;

  // The six byte report. The right stick's X and the left trigger are split over several
  // bytes.
  struct DataFormat
  {
    using StickType = Common::TVec2<u8>;
    using LeftStickRawValue = ControllerEmu::RawValue<StickType, LEFT_STICK_BITS>;
    using RightStickRawValue = ControllerEmu::RawValue<StickType, RIGHT_STICK_BITS>;

    using TriggerType = u8;
    using TriggerRawValue = ControllerEmu::RawValue<TriggerType, TRIGGER_BITS>;

    // 0-63 per axis.
    auto GetLeftStick() const { return LeftStickRawValue{StickType(lx, ly)}; }
    void SetLeftStick(const StickType& value)
    {
      lx = value.x & 0x3f;
      ly = value.y & 0x3f;
    }

    // 0-31 per axis. X is stored as bit 0 (rx1), bits 1-2 (rx2) and bits 3-4 (rx3).
    auto GetRightStick() const
    {
      const auto x = static_cast<u8>(rx1 | (rx2 << 1) | (rx3 << 3));
      return RightStickRawValue{StickType(x, ry)};
    }
    void SetRightStick(const StickType& value)
    {
      rx1 = value.x & 0b1;
      rx2 = (value.x >> 1) & 0b11;
      rx3 = (value.x >> 3) & 0b11;
      ry = value.y & 0x1f;
    }

    // 0-31. The left trigger keeps bits 0-2 in lt1 and bits 3-4 in lt2.
    auto GetLeftTrigger() const { return TriggerRawValue(static_cast<u8>(lt1 | (lt2 << 3))); }
    void SetLeftTrigger(TriggerType value)
    {
      lt1 = value & 0b111;
      lt2 = (value >> 3) & 0b11;
    }
    auto GetRightTrigger() const { return TriggerRawValue(rt); }
    void SetRightTrigger(TriggerType value) { rt = value & 0x1f; }

    // A "pressed" mask; the report stores the inverse.
    u16 GetButtons() const { return static_cast<u16>(~bt.hex); }
    void SetButtons(u16 value) { bt.hex = static_cast<u16>(~value); }

    // byte 0
    u8 lx : 6;
    u8 rx3 : 2;

    // byte 1
    u8 ly : 6;
    u8 rx2 : 2;

    // byte 2
    u8 ry : 5;
    u8 lt2 : 2;
    u8 rx1 : 1;

    // byte 3
    u8 rt : 5;
    u8 lt1 : 3;

    // bytes 4 and 5
    ButtonFormat bt;
  };
  static_assert(sizeof(DataFormat) == 6, "Wrong size");

  using DesiredState = DataFormat;

  static constexpr int CAL_STICK_BITS = 8;
  static constexpr int CAL_TRIGGER_BITS = 8;

  // The 16 calibration bytes at 0x20 (8 bit values).
  struct CalibrationData
  {
    using StickType = DataFormat::StickType;
    using TriggerType = DataFormat::TriggerType;

    using StickCalibration = ControllerEmu::ThreePointCalibration<StickType, CAL_STICK_BITS>;
    using TriggerCalibration = ControllerEmu::TwoPointCalibration<TriggerType, CAL_TRIGGER_BITS>;

    static constexpr TriggerType TRIGGER_MAX = std::numeric_limits<TriggerType>::max();

    struct StickAxis
    {
      u8 max;
      u8 min;
      u8 center;
    };

    static StickCalibration MakeStick(const StickAxis& x, const StickAxis& y)
    {
      return StickCalibration{StickType{x.min, y.min}, StickType{x.center, y.center},
                              StickType{x.max, y.max}};
    }

    auto GetLeftStick() const { return MakeStick(left_stick_x, left_stick_y); }
    auto GetRightStick() const { return MakeStick(right_stick_x, right_stick_y); }
    auto GetLeftTrigger() const { return TriggerCalibration{left_trigger_zero, TRIGGER_MAX}; }
    auto GetRightTrigger() const { return TriggerCalibration{right_trigger_zero, TRIGGER_MAX}; }

    StickAxis left_stick_x;
    StickAxis left_stick_y;
    StickAxis right_stick_x;
    StickAxis right_stick_y;

    u8 left_trigger_zero;
    u8 right_trigger_zero;

    std::array<u8, 2> checksum;
  };
  static_assert(sizeof(CalibrationData) == 16, "Wrong size");

  Classic();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;

  ControllerEmu::ControlGroup* GetGroup(ClassicGroup group);

  // Button bits (of the "pressed" mask).
  static constexpr u16 PAD_RIGHT = 0x80;
  static constexpr u16 PAD_DOWN = 0x40;
  static constexpr u16 TRIGGER_L = 0x20;
  static constexpr u16 BUTTON_MINUS = 0x10;
  static constexpr u16 BUTTON_HOME = 0x08;
  static constexpr u16 BUTTON_PLUS = 0x04;
  static constexpr u16 TRIGGER_R = 0x02;
  static constexpr u16 NOTHING = 0x01;
  static constexpr u16 BUTTON_ZL = 0x8000;
  static constexpr u16 BUTTON_B = 0x4000;
  static constexpr u16 BUTTON_Y = 0x2000;
  static constexpr u16 BUTTON_A = 0x1000;
  static constexpr u16 BUTTON_X = 0x0800;
  static constexpr u16 BUTTON_ZR = 0x0400;
  static constexpr u16 PAD_LEFT = 0x0200;
  static constexpr u16 PAD_UP = 0x0100;

  // Measured on a real Classic Controller.
  static constexpr u8 STICK_GATE_RADIUS = 0x61;

  static constexpr u8 CAL_STICK_CENTER = 0x80;
  static constexpr u8 CAL_STICK_RADIUS = 0x7f;
  static constexpr u8 CAL_STICK_RANGE = 0xff;

  static constexpr u8 LEFT_STICK_CENTER = CAL_STICK_CENTER >> (CAL_STICK_BITS - LEFT_STICK_BITS);
  static constexpr u8 LEFT_STICK_RANGE = CAL_STICK_RANGE >> (CAL_STICK_BITS - LEFT_STICK_BITS);

  static constexpr u8 RIGHT_STICK_CENTER = CAL_STICK_CENTER >> (CAL_STICK_BITS - RIGHT_STICK_BITS);
  static constexpr u8 RIGHT_STICK_RANGE = CAL_STICK_RANGE >> (CAL_STICK_BITS - RIGHT_STICK_BITS);

  static constexpr u8 TRIGGER_RANGE = 0x1F;

  static constexpr const char* BUTTONS_GROUP = _trans("Buttons");
  static constexpr const char* LEFT_STICK_GROUP = _trans("Left Stick");
  static constexpr const char* RIGHT_STICK_GROUP = _trans("Right Stick");
  static constexpr const char* TRIGGERS_GROUP = _trans("Triggers");
  static constexpr const char* DPAD_GROUP = _trans("D-Pad");

  static constexpr const char* A_BUTTON = "A";
  static constexpr const char* B_BUTTON = "B";
  static constexpr const char* X_BUTTON = "X";
  static constexpr const char* Y_BUTTON = "Y";
  static constexpr const char* ZL_BUTTON = "ZL";
  static constexpr const char* ZR_BUTTON = "ZR";
  static constexpr const char* MINUS_BUTTON = "-";
  static constexpr const char* PLUS_BUTTON = "+";
  static constexpr const char* HOME_BUTTON = "Home";

  // i18n: The left trigger button (labeled L on real controllers)
  static constexpr const char* L_DIGITAL = _trans("L");
  // i18n: The right trigger button (labeled R on real controllers)
  static constexpr const char* R_DIGITAL = _trans("R");
  // i18n: The left trigger button (labeled L on real controllers) used as an analog input
  static constexpr const char* L_ANALOG = _trans("L-Analog");
  // i18n: The right trigger button (labeled R on real controllers) used as an analog input
  static constexpr const char* R_ANALOG = _trans("R-Analog");

private:
  ControllerEmu::Buttons* m_buttons;
  ControllerEmu::MixedTriggers* m_triggers;
  ControllerEmu::Buttons* m_dpad;
  ControllerEmu::AnalogStick* m_left_stick;
  ControllerEmu::AnalogStick* m_right_stick;
};
}  // namespace WiimoteEmu
