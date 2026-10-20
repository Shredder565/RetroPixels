// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/MathUtil.h"
#include "Common/Matrix.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"
#include "InputCommon/ControllerEmu/ControlGroup/Cursor.h"
#include "InputCommon/ControllerEmu/ControlGroup/Force.h"
#include "InputCommon/ControllerEmu/ControlGroup/IMUAccelerometer.h"
#include "InputCommon/ControllerEmu/ControlGroup/IMUCursor.h"
#include "InputCommon/ControllerEmu/ControlGroup/IMUGyroscope.h"
#include "InputCommon/ControllerEmu/ControlGroup/Tilt.h"

// Motion simulation for the emulated remote and Nunchuk: turns the swing, shake, tilt and point
// inputs into a smoothly moving physical state, and that state into sensor readings.

namespace WiimoteEmu
{
using MathUtil::GRAVITY_ACCELERATION;

// Position and its derivatives (meters, m/s, m/s^2). Saved raw in savestates.
struct PositionalState
{
  Common::Vec3 position{};
  Common::Vec3 velocity{};
  Common::Vec3 acceleration{};
};

// Orientation as per-axis angles (radians) and angular velocity (radians/s).
struct RotationalState
{
  Common::Vec3 angle{};
  Common::Vec3 angular_velocity{};
};

// Pointing from motion sensors.
struct IMUCursorState
{
  // How the world is rotated around the device.
  Common::Quaternion rotation = Common::Quaternion::Identity();

  float recentered_pitch = {};
};

struct MotionState : PositionalState, RotationalState
{
  MotionState() = default;
};

// Corrects a gyroscope-integrated rotation (of the world around the device) towards what the
// accelerometer measures. accelerometer_normal can be replaced to correct from other data.
Common::Quaternion ComplementaryFilter(const Common::Quaternion& gyroscope,
                                       const Common::Vec3& accelerometer, float accel_weight,
                                       const Common::Vec3& accelerometer_normal = {0, 0, 1});

// Orientation implied by an accelerometer reading.
Common::Quaternion GetRotationFromAcceleration(const Common::Vec3& accel);

// Rotation described by an angular velocity times a time step.
Common::Quaternion GetRotationFromGyroscope(const Common::Vec3& gyro);

// Rotation matrix from per-axis angles (applied X, then Y, then Z).
Common::Matrix33 GetRotationalMatrix(const Common::Vec3& angle);

float GetPitch(const Common::Quaternion& world_rotation);
float GetRoll(const Common::Quaternion& world_rotation);
float GetYaw(const Common::Quaternion& world_rotation);

// Moves towards a target as fast as a jerk (or angular acceleration) limit allows, arriving
// with zero speed.
void ApproachPositionWithJerk(PositionalState* state, const Common::Vec3& target,
                              const Common::Vec3& max_jerk, float time_elapsed);
void ApproachAngleWithAccel(RotationalState* state, const Common::Vec3& target, float max_accel,
                            float time_elapsed);

// One simulation step of each motion input.
void EmulateShake(PositionalState* state, ControllerEmu::Shake* shake_group, float time_elapsed);
void EmulateTilt(RotationalState* state, ControllerEmu::Tilt* tilt_group, float time_elapsed);
void EmulateSwing(MotionState* state, ControllerEmu::Force* swing_group, float time_elapsed);
void EmulatePoint(MotionState* state, ControllerEmu::Cursor* ir_group,
                  const ControllerEmu::InputOverrideFunction& override_func, float time_elapsed);
void EmulateIMUCursor(IMUCursorState* state, ControllerEmu::IMUCursor* imu_ir_group,
                      ControllerEmu::IMUAccelerometer* imu_accelerometer_group,
                      ControllerEmu::IMUGyroscope* imu_gyroscope_group, float time_elapsed);

// m/s^2 to the 10-bit unsigned values of the remote and Nunchuk accelerometers.
WiimoteCommon::AccelData ConvertAccelData(const Common::Vec3& accel, u16 zero_g, u16 one_g);

}  // namespace WiimoteEmu
