// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Matrix.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"

// Byte layouts of the Wii Remote's HID reports (see WiiBrew's "Wiimote" page).
// Unless a struct says otherwise, it describes the report body that follows the report ID byte.

namespace WiimoteCommon
{
#pragma pack(push, 1)

// Any output report: the ID byte followed by the body. Bit 0 of the first body byte is the
// rumble motor in every output report.
struct OutputReportGeneric
{
  static constexpr int HEADER_SIZE = sizeof(OutputReportID);

  OutputReportID rpt_id;

  union
  {
    u8 data[1];  // The body really is longer; its length depends on the report.
    struct
    {
      u8 rumble : 1;
    };
  };
};
static_assert(sizeof(OutputReportGeneric) == 2, "Wrong size");

// 0x10
struct OutputReportRumble
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::Rumble;

  u8 rumble : 1;
};
static_assert(sizeof(OutputReportRumble) == 1, "Wrong size");

// Body shared by the simple on/off reports (0x13, 0x14, 0x19, 0x1a).
struct OutputReportEnableFeature
{
  u8 rumble : 1;
  u8 ack : 1;     // Ask for an acknowledge report.
  u8 enable : 1;  // New state of the feature.
};
static_assert(sizeof(OutputReportEnableFeature) == 1, "Wrong size");

struct OutputReportIRLogicEnable : OutputReportEnableFeature
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::IRLogicEnable;
};
static_assert(sizeof(OutputReportIRLogicEnable) == 1, "Wrong size");

struct OutputReportIRLogicEnable2 : OutputReportEnableFeature
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::IRLogicEnable2;
};
static_assert(sizeof(OutputReportIRLogicEnable2) == 1, "Wrong size");

struct OutputReportSpeakerEnable : OutputReportEnableFeature
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::SpeakerEnable;
};
static_assert(sizeof(OutputReportSpeakerEnable) == 1, "Wrong size");

struct OutputReportSpeakerMute : OutputReportEnableFeature
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::SpeakerMute;
};
static_assert(sizeof(OutputReportSpeakerMute) == 1, "Wrong size");

// 0x11
struct OutputReportLeds
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::LED;

  u8 rumble : 1;
  u8 ack : 1;
  u8 : 2;
  u8 leds : 4;  // Player LEDs 1-4 in bits 4-7 of the byte.
};
static_assert(sizeof(OutputReportLeds) == 1, "Wrong size");

// 0x12
struct OutputReportMode
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::ReportMode;

  u8 rumble : 1;
  u8 ack : 1;
  u8 continuous : 1;
  u8 : 5;
  InputReportID mode;
};
static_assert(sizeof(OutputReportMode) == 2, "Wrong size");

// 0x15
struct OutputReportRequestStatus
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::RequestStatus;

  u8 rumble : 1;
  u8 : 7;
};
static_assert(sizeof(OutputReportRequestStatus) == 1, "Wrong size");

// 0x16
struct OutputReportWriteData
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::WriteData;

  u8 rumble : 1;
  u8 : 1;
  u8 space : 2;  // AddressSpace
  u8 : 4;
  u8 i2c_rw_ignored : 1;  // The R/W bit of the i2c address; real remotes ignore it.
  u8 slave_address : 7;   // Only meaningful for the i2c spaces.
  u8 address[2];          // Big endian.
  u8 size;
  u8 data[16];
};
static_assert(sizeof(OutputReportWriteData) == 21, "Wrong size");

// 0x17
struct OutputReportReadData
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::ReadData;

  u8 rumble : 1;
  u8 : 1;
  u8 space : 2;  // AddressSpace
  u8 : 4;
  u8 i2c_rw_ignored : 1;  // The R/W bit of the i2c address; real remotes ignore it.
  u8 slave_address : 7;   // Only meaningful for the i2c spaces.
  u8 address[2];          // Big endian.
  u8 size[2];             // Big endian.
};
static_assert(sizeof(OutputReportReadData) == 6, "Wrong size");

// 0x18
struct OutputReportSpeakerData
{
  static constexpr OutputReportID REPORT_ID = OutputReportID::SpeakerData;
  static constexpr size_t DATA_SIZE = 20;

  u8 rumble : 1;
  u8 : 2;
  u8 length : 5;
  std::array<u8, DATA_SIZE> data;
};
static_assert(sizeof(OutputReportSpeakerData) == 21, "Wrong size");

// The two "core" bytes found at the start of most input reports. Besides the buttons they carry
// low bits of the accelerometer, so BUTTON_MASK selects the button bits.
union ButtonData
{
  static constexpr u16 BUTTON_MASK = 0x9f1f;

  u16 hex;

  struct
  {
    // First byte.
    u8 left : 1;
    u8 right : 1;
    u8 down : 1;
    u8 up : 1;
    u8 plus : 1;
    u8 acc_bits : 2;  // Accel X bits 0-1, or accel Z bits in the interleaved reports.
    u8 unknown : 1;

    // Second byte.
    u8 two : 1;
    u8 one : 1;
    u8 b : 1;
    u8 a : 1;
    u8 minus : 1;
    u8 acc_bits2 : 2;  // Accel Y/Z bit 1, or accel Z bits in the interleaved reports.
    u8 home : 1;
  };
};
static_assert(sizeof(ButtonData) == 2, "Wrong size");

// 0x20
struct InputReportStatus
{
  static constexpr InputReportID REPORT_ID = InputReportID::Status;

  ButtonData buttons;
  u8 battery_low : 1;
  u8 extension : 1;
  u8 speaker : 1;
  u8 ir : 1;
  u8 leds : 4;
  u8 padding2[2];
  u8 battery;

  // The battery byte is mapped linearly to a charge level. The line was fitted to the charge
  // levels the Wii Menu shows as bars.
  constexpr float GetEstimatedCharge() const
  {
    return battery * CHARGE_SLOPE / BATTERY_FULL_SCALE + CHARGE_OFFSET;
  }
  void SetEstimatedCharge(float charge)
  {
    const float scaled = (charge - CHARGE_OFFSET) / CHARGE_SLOPE * BATTERY_FULL_SCALE;
    battery = static_cast<u8>(std::lround(scaled));
  }

private:
  static constexpr u8 BATTERY_FULL_SCALE = std::numeric_limits<u8>::max();
  static constexpr float CHARGE_SLOPE = 2.46f;
  static constexpr float CHARGE_OFFSET = -0.013f;
};
static_assert(sizeof(InputReportStatus) == 6, "Wrong size");

// 0x22
struct InputReportAck
{
  static constexpr InputReportID REPORT_ID = InputReportID::Ack;

  ButtonData buttons;
  OutputReportID rpt_id;
  ErrorCode error_code;
};
static_assert(sizeof(InputReportAck) == 4, "Wrong size");

// 0x21
struct InputReportReadDataReply
{
  static constexpr InputReportID REPORT_ID = InputReportID::ReadDataReply;

  ButtonData buttons;
  u8 error : 4;
  u8 size_minus_one : 4;
  u16 address;  // Big endian.
  u8 data[16];
};
static_assert(sizeof(InputReportReadDataReply) == 21, "Wrong size");

// Accelerometer values are always handled with 10 bits per axis, whatever the report holds.
using AccelType = Common::TVec3<u16>;
using AccelData = ControllerEmu::RawValue<AccelType, 10>;

// One calibration point (0 g or 1 g) as stored in the remote's EEPROM and in the Nunchuk:
// the upper 8 bits of each axis, then one byte with the two low bits of each.
struct AccelCalibrationPoint
{
  u16 GetX() const { return Combine(x2, x1); }
  u16 GetY() const { return Combine(y2, y1); }
  u16 GetZ() const { return Combine(z2, z1); }
  auto Get() const { return AccelType{GetX(), GetY(), GetZ()}; }

  void SetX(u16 x)
  {
    x2 = High(x);
    x1 = Low(x);
  }
  void SetY(u16 y)
  {
    y2 = High(y);
    y1 = Low(y);
  }
  void SetZ(u16 z)
  {
    z2 = High(z);
    z1 = Low(z);
  }
  void Set(AccelType accel)
  {
    SetX(accel.x);
    SetY(accel.y);
    SetZ(accel.z);
  }

  u8 x2, y2, z2;
  u8 z1 : 2;
  u8 y1 : 2;
  u8 x1 : 2;
  u8 : 2;

private:
  static u16 Combine(u8 high, u8 low) { return static_cast<u16>((high << 2) | low); }
  static u8 High(u16 value) { return static_cast<u8>(value >> 2); }
  static u8 Low(u16 value) { return static_cast<u8>(value & 0b11); }
};

// The accelerometer calibration block of the remote's EEPROM (stored at 0x16 and again at 0x20).
struct AccelCalibrationData
{
  using Calibration = ControllerEmu::TwoPointCalibration<AccelType, 10>;

  auto GetCalibration() const { return Calibration(zero_g.Get(), one_g.Get()); }

  AccelCalibrationPoint zero_g;
  AccelCalibrationPoint one_g;

  u8 volume : 7;
  u8 motor : 1;
  u8 checksum;
};

}  // namespace WiimoteCommon

#pragma pack(pop)
