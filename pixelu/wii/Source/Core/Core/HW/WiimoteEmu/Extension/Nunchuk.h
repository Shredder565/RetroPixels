// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/Common.h"

#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "Core/HW/WiimoteEmu/Dynamics.h"
#include "Core/HW/WiimoteEmu/Extension/Extension.h"

namespace ControllerEmu
{
class AnalogStick;
class Buttons;
class ControlGroup;
class Force;
class Tilt;
}  // namespace ControllerEmu

// The Nunchuk: stick, C and Z buttons and a three axis accelerometer
// (WiiBrew: "Wiimote/Extension Controllers/Nunchuck").

namespace WiimoteEmu
{
enum class NunchukGroup
{
  Buttons,
  Stick,
  Tilt,
  Swing,
  Shake,
  IMUAccelerometer,
};

class Nunchuk : public Extension1stParty
{
public:
  // Byte 5 of the report: active-low buttons and the accelerometer's two low bits per axis.
  union ButtonFormat
  {
    u8 hex;

    struct
    {
      u8 z : 1;
      u8 c : 1;
      u8 acc_x_lsb : 2;
      u8 acc_y_lsb : 2;
      u8 acc_z_lsb : 2;
    };
  };
  static_assert(sizeof(ButtonFormat) == 1, "Wrong size");

  // The six byte report.
  struct DataFormat
  {
    using StickType = Common::TVec2<u8>;
    using StickRawValue = ControllerEmu::RawValue<StickType, 8>;

    using AccelType = WiimoteCommon::AccelType;
    using AccelData = WiimoteCommon::AccelData;

    auto GetStick() const { return StickRawValue(StickType(jx, jy)); }

    // Accelerometer axes are 10 bits: eight in their own byte, two in byte 5.
    u16 GetAccelX() const { return Join(ax, bt.acc_x_lsb); }
    u16 GetAccelY() const { return Join(ay, bt.acc_y_lsb); }
    u16 GetAccelZ() const { return Join(az, bt.acc_z_lsb); }
    auto GetAccel() const { return AccelData{AccelType{GetAccelX(), GetAccelY(), GetAccelZ()}}; }

    void SetAccelX(u16 val)
    {
      ax = High(val);
      bt.acc_x_lsb = Low(val);
    }
    void SetAccelY(u16 val)
    {
      ay = High(val);
      bt.acc_y_lsb = Low(val);
    }
    void SetAccelZ(u16 val)
    {
      az = High(val);
      bt.acc_z_lsb = Low(val);
    }
    void SetAccel(const AccelType& accel)
    {
      SetAccelX(accel.x);
      SetAccelY(accel.y);
      SetAccelZ(accel.z);
    }

    // Buttons as a "pressed" mask (the report stores them inverted).
    u8 GetButtons() const { return static_cast<u8>(~bt.hex & BUTTON_MASK); }
    void SetButtons(u8 value)
    {
      bt.hex = static_cast<u8>((bt.hex | BUTTON_MASK) ^ (value & BUTTON_MASK));
    }

    u8 jx;
    u8 jy;

    // Upper eight bits of each accelerometer axis.
    u8 ax;
    u8 ay;
    u8 az;

    ButtonFormat bt;

  private:
    static constexpr u8 BUTTON_MASK = 0x03;  // C and Z

    static u16 Join(u8 high, u8 low) { return static_cast<u16>((high << 2) | low); }
    static u8 High(u16 value) { return static_cast<u8>(value >> 2); }
    static u8 Low(u16 value) { return static_cast<u8>(value & 0b11); }
  };
  static_assert(sizeof(DataFormat) == 6, "Wrong size");

  using DesiredState = DataFormat;

  // The 16 calibration bytes at 0x20.
  struct CalibrationData
  {
    using StickType = DataFormat::StickType;
    using StickCalibration = ControllerEmu::ThreePointCalibration<StickType, 8>;

    using AccelType = WiimoteCommon::AccelType;
    using AccelCalibration = ControllerEmu::TwoPointCalibration<AccelType, 10>;

    struct Stick
    {
      u8 max;
      u8 min;
      u8 center;
    };

    auto GetStick() const
    {
      return StickCalibration(StickType{stick_x.min, stick_y.min},
                              StickType{stick_x.center, stick_y.center},
                              StickType{stick_x.max, stick_y.max});
    }
    auto GetAccel() const { return AccelCalibration(accel_zero_g.Get(), accel_one_g.Get()); }

    WiimoteCommon::AccelCalibrationPoint accel_zero_g;
    WiimoteCommon::AccelCalibrationPoint accel_one_g;

    Stick stick_x;
    Stick stick_y;

    std::array<u8, 2> checksum;
  };
  static_assert(sizeof(CalibrationData) == 16, "Wrong size");

  Nunchuk();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;
  void DoState(PointerWrap& p) override;

  ControllerEmu::ControlGroup* GetGroup(NunchukGroup group);

  void LoadDefaults() override;

  static constexpr u8 BUTTON_C = 0x02;
  static constexpr u8 BUTTON_Z = 0x01;

  // Measured on a real Nunchuk.
  static constexpr u8 ACCEL_ZERO_G = 0x80;
  static constexpr u8 ACCEL_ONE_G = 0xB3;
  static constexpr u8 STICK_GATE_RADIUS = 0x60;

  static constexpr u8 STICK_CENTER = 0x80;
  static constexpr u8 STICK_RADIUS = 0x7F;
  static constexpr u8 STICK_RANGE = 0xFF;

  static constexpr const char* BUTTONS_GROUP = _trans("Buttons");
  static constexpr const char* STICK_GROUP = _trans("Stick");
  static constexpr const char* ACCELEROMETER_GROUP = "IMUAccelerometer";

  static constexpr const char* C_BUTTON = "C";
  static constexpr const char* Z_BUTTON = "Z";

private:
  ControllerEmu::Tilt* m_tilt;
  ControllerEmu::Force* m_swing;
  ControllerEmu::Shake* m_shake;
  ControllerEmu::Buttons* m_buttons;
  ControllerEmu::AnalogStick* m_stick;
  ControllerEmu::IMUAccelerometer* m_imu_accelerometer;

  // Motion simulation state (saved in savestates in this order: swing, tilt, shake).
  MotionState m_swing_state;
  RotationalState m_tilt_state;
  PositionalState m_shake_state;
};
}  // namespace WiimoteEmu
