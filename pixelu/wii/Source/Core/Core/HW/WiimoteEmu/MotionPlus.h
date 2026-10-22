// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"
#include "Core/HW/WiimoteEmu/Dynamics.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "Core/HW/WiimoteEmu/I2CBus.h"

// The Wii MotionPlus: a three axis gyroscope that plugs into the extension port and has an
// extension port of its own. Until activated it sits on i2c slave 0x53 and passes other traffic
// through; once activated it takes over slave 0x52 and can interleave its data with that of a
// Nunchuk or Classic Controller ("passthrough"). Behaviour and formats follow WiiBrew's
// "Wiimote/Extension Controllers/Wii Motion Plus" page.

namespace WiimoteEmu
{
class MotionPlus : public Extension
{
public:
  // Values written to 0xfe to activate (and later read back from the identifier). Any other
  // value deactivates.
  enum class PassthroughMode : u8
  {
    // Active, without passthrough.
    Disabled = 0x04,
    Nunchuk = 0x05,
    Classic = 0x07,
  };

#pragma pack(push, 1)
  // Zero points, scales and full-scale rotation rate (in units of 6 deg/s) of each axis.
  struct CalibrationBlock
  {
    Common::BigEndianValue<u16> yaw_zero;
    Common::BigEndianValue<u16> roll_zero;
    Common::BigEndianValue<u16> pitch_zero;
    Common::BigEndianValue<u16> yaw_scale;
    Common::BigEndianValue<u16> roll_scale;
    Common::BigEndianValue<u16> pitch_scale;
    u8 degrees_div_6;
  };

  struct CalibrationBlocks
  {
    using GyroType = Common::TVec3<u16>;
    using SlowType = Common::TVec3<bool>;

    struct RelevantCalibration
    {
      ControllerEmu::TwoPointCalibration<GyroType, 16> value;
      Common::TVec3<u16> degrees;
    };

    // Picks, per axis, the slow or fast block matching the data's speed flags.
    RelevantCalibration GetRelevantCalibration(SlowType is_slow) const;

    CalibrationBlock fast;
    CalibrationBlock slow;
  };

  // The 32 bytes at 0x20: two blocks, each followed by a uid byte and half of a CRC-32.
  struct CalibrationData
  {
    void UpdateChecksum();

    CalibrationBlock fast;
    u8 uid_1;
    Common::BigEndianValue<u16> crc32_msb;
    CalibrationBlock slow;
    u8 uid_2;
    Common::BigEndianValue<u16> crc32_lsb;
  };
  static_assert(sizeof(CalibrationData) == 0x20, "Wrong size");

  // The six bytes of a MotionPlus data report.
  struct DataFormat
  {
    using GyroType = CalibrationBlocks::GyroType;
    using SlowType = CalibrationBlocks::SlowType;
    using GyroRawValue = ControllerEmu::RawValue<GyroType, 14>;

    struct Data
    {
      // Radians per second about x, y and z (right-hand rule) using the given calibration.
      Common::Vec3 GetAngularVelocity(const CalibrationBlocks&) const;

      GyroRawValue gyro;
      SlowType is_slow;
    };

    auto GetData() const
    {
      const auto join = [](u8 low, u8 high) { return static_cast<u16>(low | (high << 8)); };
      return Data{
          GyroRawValue{GyroType(join(pitch1, pitch2), join(roll1, roll2), join(yaw1, yaw2))},
          SlowType(pitch_slow, roll_slow, yaw_slow)};
    }

    // Each rate is 14 bits: the low 8 in *1, the high 6 in *2.
    u8 yaw1;
    u8 roll1;
    u8 pitch1;

    u8 pitch_slow : 1;
    u8 yaw_slow : 1;
    u8 yaw2 : 6;

    u8 extension_connected : 1;
    u8 roll_slow : 1;
    u8 roll2 : 6;

    u8 zero : 1;
    u8 is_mp_data : 1;
    u8 pitch2 : 6;
  };
  static_assert(sizeof(DataFormat) == 6, "Wrong size");
#pragma pack(pop)

  static constexpr u8 INACTIVE_DEVICE_ADDR = 0x53;
  static constexpr u8 ACTIVE_DEVICE_ADDR = 0x52;
  static constexpr u8 PASSTHROUGH_MODE_OFFSET = 0xfe;

  static constexpr int CALIBRATION_BITS = 16;

  static constexpr u16 CALIBRATION_ZERO = 1 << (CALIBRATION_BITS - 1);
  // Close to the values of a typical real MotionPlus.
  static constexpr u16 CALIBRATION_SCALE_OFFSET = 0x4400;
  static constexpr u16 CALIBRATION_FAST_SCALE_DEGREES = 0x4b0;
  static constexpr u16 CALIBRATION_SLOW_SCALE_DEGREES = 0x10e;

  static constexpr int BITS_OF_PRECISION = 14;
  static constexpr s32 ZERO_VALUE = CALIBRATION_ZERO >> (CALIBRATION_BITS - BITS_OF_PRECISION);
  static constexpr s32 MAX_VALUE = (1 << BITS_OF_PRECISION) - 1;

  static constexpr u16 VALUE_SCALE =
      (CALIBRATION_SCALE_OFFSET >> (CALIBRATION_BITS - BITS_OF_PRECISION));
  static constexpr float VALUE_SCALE_DEGREES = VALUE_SCALE / float(MathUtil::TAU) * 360;

  static constexpr float SLOW_SCALE = VALUE_SCALE_DEGREES / CALIBRATION_SLOW_SCALE_DEGREES;
  static constexpr float FAST_SCALE = VALUE_SCALE_DEGREES / CALIBRATION_FAST_SCALE_DEGREES;

  static_assert(ZERO_VALUE == 1 << (BITS_OF_PRECISION - 1),
                "The rate limits below assume the zero point is mid-range.");

  static constexpr u16 SENSOR_RANGE = 1 << (BITS_OF_PRECISION - 1);
  static constexpr float SLOW_MAX_RAD_PER_SEC = SENSOR_RANGE / SLOW_SCALE;
  static constexpr float FAST_MAX_RAD_PER_SEC = SENSOR_RANGE / FAST_SCALE;

  MotionPlus();

  void BuildDesiredExtensionState(DesiredExtensionState* target_state) override;
  void Update(const DesiredExtensionState& target_state) override;
  void Reset() override;
  void DoState(PointerWrap& p) override;

  // The port at the end of the MotionPlus.
  ExtensionPort& GetExtPort();

  // Raw sensor data for an angular velocity in radians/s (right-hand rule).
  static MotionPlus::DataFormat::Data GetGyroscopeData(const Common::Vec3& angular_velocity);
  // Raw sensor data at rest.
  static MotionPlus::DataFormat::Data GetDefaultGyroscopeData();

  // Builds the next data report in the register file.
  void PrepareInput(const MotionPlus::DataFormat::Data& gyroscope_data);

  // The bit shuffles applied to six bytes of passthrough extension data, and their inverse.
  static void ApplyPassthroughModifications(PassthroughMode, u8* data);
  static void ReversePassthroughModifications(PassthroughMode, u8* data);

private:
  // Register 0xf7: progress of the authentication challenge.
  enum class ChallengeState : u8
  {
    // Not a real MotionPlus value: marks the unresponsive period right after activation.
    Activating = 0x00,

    PreparingX = 0x02,
    ParameterXReady = 0x0e,
    PreparingY = 0x14,
    ParameterYReady = 0x1a,
  };

  enum class ActivationStatus
  {
    Inactive,
    Activating,
    Deactivating,
    Active,
  };

#pragma pack(push, 1)
  struct Register
  {
    // 0x00: report data.
    std::array<u8, 21> controller_data;
    u8 unknown_0x15[11];

    // 0x20: gyroscope calibration (CalibrationData).
    std::array<u8, 0x20> calibration_data;

    // 0x40: calibration copied from the extension on the passthrough port.
    std::array<u8, 0x10> passthrough_ext_calib;

    // 0x50: challenge parameters x and y (little endian).
    std::array<u8, 0x40> challenge_data;

    u8 unknown_0x90[0x60];

    // 0xf0: any write while active deactivates (and initializes the passthrough extension).
    u8 init_trigger;

    // 0xf1: 0 or 1, selects which y parameter the game wants.
    u8 challenge_type;

    // 0xf2: written with 0 by games around calibration.
    u8 calibration_trigger;

    u8 unknown_0xf3[3];

    // 0xf6: identifier byte 4 of the passthrough extension.
    u8 passthrough_ext_id_4;

    // 0xf7: ChallengeState. A real MotionPlus steps through intermediate values; the emulated
    // one jumps straight to the "ready" values.
    ChallengeState challenge_state;

    // 0xf8: identifier bytes 0 and 5 of the passthrough extension.
    u8 passthrough_ext_id_0;
    u8 passthrough_ext_id_5;

    // 0xfa: identifier.
    std::array<u8, 6> ext_identifier;
  };
#pragma pack(pop)
  static_assert(0x100 == sizeof(Register), "Wrong size");

  void Activate();
  void Deactivate();
  void OnPassthroughModeWrite();

  ActivationStatus GetActivationStatus() const;
  PassthroughMode GetPassthroughMode() const;

  int BusRead(u8 slave_addr, u8 addr, int count, u8* data_out) override;
  int BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in) override;

  bool ReadDeviceDetectPin() const override;

  Register m_reg_data = {};

  // Updates left until the current (de)activation or challenge step finishes.
  u8 m_progress_timer = {};

  // The bus and port at the far end.
  I2CBus m_i2c_bus;
  ExtensionPort m_extension_port{&m_i2c_bus};
};
}  // namespace WiimoteEmu
