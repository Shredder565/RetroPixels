// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"

#include <array>

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/DesiredExtensionState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControlGroup/Force.h"
#include "InputCommon/ControllerEmu/ControlGroup/IMUAccelerometer.h"
#include "InputCommon/ControllerEmu/ControlGroup/Tilt.h"

namespace WiimoteEmu
{
namespace
{
constexpr std::array<u8, 6> NUNCHUK_ID = {0x00, 0x00, 0xa4, 0x20, 0x00, 0x00};

// Report bit of each entry of the Buttons group, in group order.
constexpr std::array<u8, 2> BUTTON_BITS = {Nunchuk::BUTTON_C, Nunchuk::BUTTON_Z};

constexpr float UPDATE_PERIOD = 1.f / ::Wiimote::UPDATE_FREQ;
}  // namespace

Nunchuk::Nunchuk() : Extension1stParty(_trans("Nunchuk"))
{
  using Translatability = ControllerEmu::Translatability;

  m_buttons = new ControllerEmu::Buttons(BUTTONS_GROUP);
  groups.emplace_back(m_buttons);
  m_buttons->AddInput(Translatability::DoNotTranslate, C_BUTTON);
  m_buttons->AddInput(Translatability::DoNotTranslate, Z_BUTTON);

  m_stick = new ControllerEmu::OctagonAnalogStick(
      STICK_GROUP, ControlState(STICK_GATE_RADIUS) / STICK_RADIUS);
  groups.emplace_back(m_stick);

  // Shaking defaults to the opposite direction of the remote's, which Donkey Kong Country
  // Returns needs to tell the two apart.
  m_shake = new ControllerEmu::Shake(_trans("Shake"), -1);
  groups.emplace_back(m_shake);

  m_tilt = new ControllerEmu::Tilt(_trans("Tilt"));
  groups.emplace_back(m_tilt);

  m_swing = new ControllerEmu::Force(_trans("Swing"));
  groups.emplace_back(m_swing);

  m_imu_accelerometer =
      new ControllerEmu::IMUAccelerometer(ACCELEROMETER_GROUP, _trans("Accelerometer"));
  groups.emplace_back(m_imu_accelerometer);
}

void Nunchuk::BuildDesiredExtensionState(DesiredExtensionState* target_state)
{
  DataFormat report = {};

  const auto stick = m_stick->GetState(m_input_override_function);
  report.jx = ControllerEmu::MapFloat<u8>(stick.x, STICK_CENTER, 0, STICK_RANGE);
  report.jy = ControllerEmu::MapFloat<u8>(stick.y, STICK_CENTER, 0, STICK_RANGE);

  u8 pressed = 0;
  m_buttons->GetState(&pressed, BUTTON_BITS.data(), m_input_override_function);
  report.SetButtons(pressed);

  EmulateSwing(&m_swing_state, m_swing, UPDATE_PERIOD);
  EmulateTilt(&m_tilt_state, m_tilt, UPDATE_PERIOD);
  EmulateShake(&m_shake_state, m_shake, UPDATE_PERIOD);

  // Gravity (or a real accelerometer's reading) plus swing acceleration, seen through the
  // tilt and swing rotations, plus shaking.
  const Common::Matrix33 orientation =
      GetRotationalMatrix(-m_tilt_state.angle) * GetRotationalMatrix(-m_swing_state.angle);
  const Common::Vec3 resting = m_imu_accelerometer->GetState().value_or(
      Common::Vec3(0, 0, float(GRAVITY_ACCELERATION)));

  Common::Vec3 accel = orientation * (m_swing_state.acceleration + resting);
  accel += m_shake_state.acceleration;
  accel = Wiimote::OverrideVec3(m_imu_accelerometer, accel, m_input_override_function);

  // The calibration holds 8 bit values; the data has 10.
  const WiimoteCommon::AccelData sensor =
      ConvertAccelData(accel, ACCEL_ZERO_G << 2, ACCEL_ONE_G << 2);
  report.SetAccel(sensor.value);

  target_state->data = report;
}

void Nunchuk::Update(const DesiredExtensionState& target_state)
{
  DefaultExtensionUpdate<DataFormat>(&m_reg, target_state);
}

void Nunchuk::Reset()
{
  EncryptedExtension::Reset();

  m_reg.identifier = NUNCHUK_ID;

  // The shake state is deliberately left alone, as it always has been.
  m_swing_state = {};
  m_tilt_state = {};

  constexpr u8 STICK_MAX = STICK_CENTER + STICK_GATE_RADIUS;
  constexpr u8 STICK_MIN = STICK_CENTER - STICK_GATE_RADIUS;

  // Zero-g x, y, z and their low bits, one-g likewise, then max/min/center of each stick
  // axis and a two byte checksum.
  m_reg.calibration = {ACCEL_ZERO_G, ACCEL_ZERO_G, ACCEL_ZERO_G, 0x00,
                       ACCEL_ONE_G,  ACCEL_ONE_G,  ACCEL_ONE_G,  0x00,
                       STICK_MAX,    STICK_MIN,    STICK_CENTER, STICK_MAX,
                       STICK_MIN,    STICK_CENTER, 0x00,         0x00};

  UpdateCalibrationDataChecksum(m_reg.calibration, CALIBRATION_CHECKSUM_BYTES);
}

ControllerEmu::ControlGroup* Nunchuk::GetGroup(NunchukGroup group)
{
  switch (group)
  {
  case NunchukGroup::Buttons:
    return m_buttons;
  case NunchukGroup::Stick:
    return m_stick;
  case NunchukGroup::Tilt:
    return m_tilt;
  case NunchukGroup::Swing:
    return m_swing;
  case NunchukGroup::Shake:
    return m_shake;
  case NunchukGroup::IMUAccelerometer:
    return m_imu_accelerometer;
  }
  ASSERT(false);
  return nullptr;
}

void Nunchuk::DoState(PointerWrap& p)
{
  EncryptedExtension::DoState(p);

  p.Do(m_swing_state);
  p.Do(m_tilt_state);
  p.Do(m_shake_state);
}

void Nunchuk::LoadDefaults()
{
#ifndef ANDROID
  // WASD for the stick, with a square calibration to suit keys.
  const char* const stick_keys[] = {"W", "S", "A", "D"};
  for (int i = 0; i != 4; ++i)
    m_stick->SetControlExpression(i, stick_keys[i]);
  m_stick->SetCalibrationFromGate(ControllerEmu::SquareStickGate(1.0));

#ifdef _WIN32
  const char* const c_key = "LCONTROL";
  const char* const z_key = "LSHIFT";
#elif __APPLE__
  const char* const c_key = "`Left Control`";
  const char* const z_key = "`Left Shift`";
#else
  const char* const c_key = "Control_L";
  const char* const z_key = "Shift_L";
#endif
  m_buttons->SetControlExpression(0, c_key);
  m_buttons->SetControlExpression(1, z_key);

#ifdef __APPLE__
  const char* const shake_key = "`Middle Click`";
#else
  const char* const shake_key = "`Click 2`";
#endif
  for (int axis = 0; axis != 3; ++axis)
    m_shake->SetControlExpression(axis, shake_key);
#endif
}
}  // namespace WiimoteEmu
