// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Dynamics.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "Common/MathUtil.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"

// Note on precision: these results feed savestates, movies and netplay, so every expression
// keeps the float/double mix of the established behaviour. Where a double is produced and
// stored into a float, the narrowing is spelled out with float(...).

namespace
{
// Distance covered while braking to a standstill with a jerk of at most `jerk`, starting at
// speed v and acceleration a: first the acceleration is ramped to zero, then the remaining
// speed is removed by a symmetric jerk profile (constant-jerk motion:
// x = v t + a t^2 / 2 + j t^3 / 6).
double BrakingDistance(double v, double a, double jerk)
{
  // Work with a non-negative speed and undo the mirroring at the end.
  const int mirror = v < 0 ? -1 : 1;
  const double speed = v * mirror;
  const double accel = a * mirror;

  const double ramp_time = accel / jerk;
  const double ramp_distance =
      std::pow(accel, 3) / (3 * jerk * jerk) + (accel * speed) / jerk;

  const double speed_after_ramp = speed + accel * std::abs(ramp_time) -
                                  std::copysign(jerk * ramp_time * ramp_time / 2, ramp_time);

  const double stop_distance =
      std::copysign(std::pow(std::abs(speed_after_ramp), 3.0 / 2), speed_after_ramp) /
      std::sqrt(jerk);

  return (ramp_distance + stop_distance) * mirror;
}

// Distance covered while braking from speed v with a constant deceleration of max_accel.
double BrakingDistance(double v, double max_accel)
{
  return v * v / (2 * std::copysign(max_accel, v));
}

Common::Vec3 BrakingDistances(const Common::Vec3& speed, float max_accel)
{
  return Common::Vec3(float(BrakingDistance(speed.x, max_accel)),
                      float(BrakingDistance(speed.y, max_accel)),
                      float(BrakingDistance(speed.z, max_accel)));
}

Common::Vec3 BrakingDistances(const Common::Vec3& speed, const Common::Vec3& accel,
                              const Common::Vec3& jerk)
{
  return Common::Vec3(float(BrakingDistance(speed.x, accel.x, jerk.x)),
                      float(BrakingDistance(speed.y, accel.y, jerk.y)),
                      float(BrakingDistance(speed.z, accel.z, jerk.z)));
}

// Where a world-space direction ends up for a device rotated by `world_rotation`.
Common::Vec3 DeviceUp(const Common::Quaternion& world_rotation)
{
  return world_rotation * Common::Vec3{0, 0, 1};
}
}  // namespace

namespace WiimoteEmu
{
Common::Quaternion ComplementaryFilter(const Common::Quaternion& gyroscope,
                                       const Common::Vec3& accelerometer, float accel_weight,
                                       const Common::Vec3& accelerometer_normal)
{
  const Common::Vec3 expected = gyroscope * accelerometer_normal;
  const Common::Vec3 measured = accelerometer.Normalized();

  const float cos_error = measured.Dot(expected);

  // Nothing to correct when both agree exactly, and no defined axis when they are opposite.
  const float abs_cos_error = std::abs(cos_error);
  if (!(abs_cos_error > 0 && abs_cos_error < 1))
    return gyroscope;

  const Common::Vec3 correction_axis = expected.Cross(measured).Normalized();
  const float correction = std::acos(cos_error) * accel_weight;
  return Common::Quaternion::Rotate(correction, correction_axis) * gyroscope;
}

void EmulateShake(PositionalState* state, ControllerEmu::Shake* const shake_group,
                  float time_elapsed)
{
  // The remote oscillates between +target and -target.
  Common::Vec3 goal = shake_group->GetState() * float(shake_group->GetIntensity() / 2);

  // Head for the opposite extreme once moving away from this one or halfway there.
  for (std::size_t axis = 0; axis != goal.data.size(); ++axis)
  {
    const bool moving_away = state->velocity.data[axis] * std::copysign(1.f, goal.data[axis]) < 0;
    const bool past_half = state->position.data[axis] / goal.data[axis] > 0.5;
    if (moving_away || past_half)
      goal.data[axis] *= -1;
  }

  // One half period goes from one extreme to the other.
  const double half_period = 1 / shake_group->GetFrequency() / 2;
  const double quarter_period_cubed = std::pow(half_period / 2, 3);

  Common::Vec3 jerk_limit;
  for (std::size_t axis = 0; axis != goal.data.size(); ++axis)
  {
    const float reach =
        std::max(std::abs(goal.data[axis]), std::abs(state->position.data[axis]));
    jerk_limit.data[axis] = float(reach / quarter_period_cubed);
  }

  ApproachPositionWithJerk(state, goal, jerk_limit, time_elapsed);
}

void EmulateTilt(RotationalState* state, ControllerEmu::Tilt* const tilt_group, float time_elapsed)
{
  const auto stick = tilt_group->GetState();

  // Full deflection is a half turn.
  const double roll = stick.x * MathUtil::PI;
  const double pitch = stick.y * MathUtil::PI;
  const Common::Vec3 goal(float(pitch), float(-roll), 0);

  // Take the short way round: unwrap the current angle when the goal is over half a turn away.
  for (std::size_t axis = 0; axis != goal.data.size(); ++axis)
  {
    float& current = state->angle.data[axis];
    if (std::abs(current - goal.data[axis]) > float(MathUtil::PI))
      current = float(current - std::copysign(MathUtil::TAU, double(current)));
  }

  const double rate = tilt_group->GetMaxRotationalVelocity();
  const double accel_limit = std::pow(rate, 2) / MathUtil::TAU;

  ApproachAngleWithAccel(state, goal, float(accel_limit), time_elapsed);
}

void EmulateSwing(MotionState* state, ControllerEmu::Force* swing_group, float time_elapsed)
{
  const Common::Vec3 input = swing_group->GetState();
  const float reach = float(swing_group->GetMaxDistance());
  const float twist = float(swing_group->GetTwistAngle());

  // The group's X/Y plane is the remote's X/Z plane, and remote +X points left.
  const Common::Vec3 goal{-input.x, -input.z, input.y};

  // Speeds blend from the return speed (at rest) to the swing speed (at full reach). X and Z
  // share one blend so movement around the circle stays even.
  const float sideways = Common::Vec2{goal.x, goal.z}.Length();
  const float forwards = std::abs(goal.y);
  const Common::Vec3 progress = Common::Vec3{sideways, forwards, sideways} / reach;

  const float return_speed = float(swing_group->GetReturnSpeed());
  const float swing_speed = float(swing_group->GetSpeed());
  const Common::Vec3 speed =
      MathUtil::Lerp(Common::Vec3{1, 1, 1} * return_speed, Common::Vec3{1, 1, 1} * swing_speed,
                     progress);

  // The jerk that reaches `speed` over a one meter move.
  const Common::Vec3 jerk_limit = speed * speed * speed * 4;

  // Twisting finishes in about the same time as the move.
  const float twist_accel = twist * speed.x * speed.x;

  const Common::Vec3 twist_goal = Common::Vec3{-goal.z, 0, goal.x} / reach * twist;

  // Twice the acceleration avoids unwanted stabs in Skyward Sword.
  ApproachAngleWithAccel(state, twist_goal, twist_accel * 2, time_elapsed);

  // Stop twisting further once X or Z is past the limit.
  for (const std::size_t axis : {std::size_t{0}, std::size_t{2}})
  {
    const float angle = state->angle.data[axis];
    const bool past_limit = std::abs(angle / twist) > 1;
    if (past_limit && MathUtil::Sign(state->angular_velocity.data[axis]) == MathUtil::Sign(angle))
      state->angular_velocity.data[axis] = 0;
  }

  // An outstretched arm pulls the remote back as it twists.
  const float lean = std::max(std::abs(state->angle.x), std::abs(state->angle.z));
  const float pull_back = (1 - std::cos(lean)) * reach;

  ApproachPositionWithJerk(state, goal + Common::Vec3{0, pull_back, 0}, jerk_limit, time_elapsed);

  // Keep the sideways position inside the configured circle.
  const float radial = Common::Vec2{state->position.x, state->position.z}.Length() / reach;
  if (radial > 1)
  {
    for (const std::size_t axis : {std::size_t{0}, std::size_t{2}})
    {
      state->position.data[axis] /= radial;
      state->velocity.data[axis] = 0;
      state->acceleration.data[axis] = 0;
    }
  }

  // Forward/backward is limited to the reach, with extra room behind for the back swing.
  const float depth = state->position.y / reach;
  const float max_depth = 2 - std::cos(twist);
  if (depth < -1 || depth > max_depth)
  {
    state->position.y = std::clamp(state->position.y, -1.f * reach, max_depth * reach);
    state->acceleration.y = 0;
    state->velocity.y = 0;
  }
}

WiimoteCommon::AccelData ConvertAccelData(const Common::Vec3& accel, u16 zero_g, u16 one_g)
{
  const Common::Vec3 counts = accel * (one_g - zero_g) / float(GRAVITY_ACCELERATION);

  // The sensors report 10 bit unsigned values.
  const auto to_sensor = [zero_g](float value) {
    return u16(std::clamp(std::lround(value + zero_g), 0l, long{0x3ff}));
  };

  return WiimoteCommon::AccelData({to_sensor(counts.x), to_sensor(counts.y), to_sensor(counts.z)});
}

void EmulatePoint(MotionState* state, ControllerEmu::Cursor* ir_group,
                  const ControllerEmu::InputOverrideFunction& override_func, float time_elapsed)
{
  const auto cursor = ir_group->GetState(true, override_func);

  if (!cursor.IsVisible())
  {
    // Put the sensor bar far behind the remote so the camera sees nothing.
    *state = {};
    state->position = {0, -1000, 0};
    return;
  }

  // Two meters from the bar (Nintendo suggests one to three).
  constexpr float VIEWING_DISTANCE = 2.f;

  // With the bar below the screen the vertical offset applies the other way round, which
  // keeps the cursor behaving the same.
  const bool bar_above = Config::Get(Config::SYSCONF_SENSOR_BAR_POSITION) != 0;
  const float elevation = float(ir_group->GetVerticalOffset() * (bar_above ? 1 : -1));

  const float half_yaw = float(ir_group->GetTotalYaw() / 2);
  const float half_pitch = float(ir_group->GetTotalPitch() / 2);

  // The position is set directly; only the aim is smoothed.
  state->position = {0, VIEWING_DISTANCE, -elevation};
  state->velocity = {};
  state->acceleration = {};

  const Common::Vec3 aim(float(half_pitch * -cursor.y), 0, float(half_yaw * -cursor.x));

  // Fast enough to look instant, slow enough to keep MotionPlus data in step.
  constexpr float AIM_ACCEL = float(MathUtil::TAU * 8);

  ApproachAngleWithAccel(state, aim, AIM_ACCEL, time_elapsed);
}

void ApproachAngleWithAccel(RotationalState* state, const Common::Vec3& angle_target,
                            float max_accel, float time_elapsed)
{
  const Common::Vec3 braking = BrakingDistances(state->angular_velocity, max_accel);

  const Common::Vec3 remaining = angle_target - state->angle;
  // Speed up while braking would still fall short, slow down otherwise.
  const Common::Vec3 accel = MathUtil::Sign(remaining - braking) * max_accel;

  const float dt = time_elapsed;
  state->angular_velocity += accel * dt;
  const Common::Vec3 step = state->angular_velocity * dt + accel * dt * dt / 2;

  for (std::size_t axis = 0; axis != remaining.data.size(); ++axis)
  {
    const bool arrived = std::abs(remaining.data[axis]) < 0.0001;
    const bool overshoot = step.data[axis] / remaining.data[axis] > 1.0;
    if (!arrived && !overshoot)
    {
      state->angle.data[axis] += step.data[axis];
      continue;
    }

    // Land exactly on the target with the speed that gets there in this step.
    state->angular_velocity.data[axis] =
        (angle_target.data[axis] - state->angle.data[axis]) / dt;
    state->angle.data[axis] = angle_target.data[axis];
  }
}

void EmulateIMUCursor(IMUCursorState* state, ControllerEmu::IMUCursor* imu_ir_group,
                      ControllerEmu::IMUAccelerometer* imu_accelerometer_group,
                      ControllerEmu::IMUGyroscope* imu_gyroscope_group, float time_elapsed)
{
  const auto gyro = imu_gyroscope_group->GetState();

  // Settings are read first: their expressions may have side effects.
  if (!imu_ir_group->enabled.GetValue() || !gyro.has_value())
  {
    *state = {};
    return;
  }

  // Integrate the gyroscope (the state rotates the world, hence the negation).
  state->rotation = GetRotationFromGyroscope(*gyro * -1 * time_elapsed) * state->rotation;

  // Pull towards the accelerometer's idea of "down" to cancel drift.
  const float weight = float(imu_ir_group->GetAccelWeight());
  const Common::Vec3 accel = imu_accelerometer_group->GetState().value_or(Common::Vec3{});
  if (accel.LengthSquared() != 0)
    state->rotation = ComplementaryFilter(state->rotation, accel, weight);

  const float yaw = GetYaw(state->rotation);
  const float yaw_limit = float(imu_ir_group->GetTotalYaw() / 2);
  float wanted_yaw = std::clamp(yaw, -yaw_limit, yaw_limit);

  // The first control of the group is "Recenter".
  if (imu_ir_group->controls[0]->GetState<bool>())
  {
    wanted_yaw = 0;
    state->recentered_pitch = GetPitch(state->rotation);
  }

  if (wanted_yaw != yaw)
    state->rotation *= Common::Quaternion::RotateZ(wanted_yaw - yaw);

  // Keep the quaternion unit length despite rounding.
  state->rotation = state->rotation.Normalized();
}

void ApproachPositionWithJerk(PositionalState* state, const Common::Vec3& position_target,
                              const Common::Vec3& max_jerk, float time_elapsed)
{
  const Common::Vec3 braking =
      BrakingDistances(state->velocity, state->acceleration, max_jerk);

  const Common::Vec3 remaining = position_target - state->position;
  const Common::Vec3 jerk = MathUtil::Sign(remaining - braking) * max_jerk;

  const float dt = time_elapsed;
  state->acceleration += jerk * dt;
  state->velocity += state->acceleration * dt + jerk * dt * dt / 2;

  const Common::Vec3 step =
      state->velocity * dt + state->acceleration * dt * dt / 2 + jerk * dt * dt * dt / 6;

  for (std::size_t axis = 0; axis != remaining.data.size(); ++axis)
  {
    // Snap to the target instead of overshooting it.
    if (step.data[axis] / remaining.data[axis] > 1.0)
    {
      state->position.data[axis] = position_target.data[axis];
      state->velocity.data[axis] = 0;
      state->acceleration.data[axis] = 0;
    }
    else
    {
      state->position.data[axis] += step.data[axis];
    }
  }
}

Common::Quaternion GetRotationFromAcceleration(const Common::Vec3& accel)
{
  constexpr Common::Vec3 UP{0, 0, 1};
  const Common::Vec3 down = accel.Normalized();

  const Common::Vec3 axis = down.Cross(UP);
  const float angle = std::acos(down.Dot(UP));

  // Straight up or down has no cross product; any horizontal axis will do.
  const Common::Vec3 safe_axis = axis.LengthSquared() != 0 ? axis.Normalized() :
                                                             Common::Vec3{0, 1, 0};
  return Common::Quaternion::Rotate(angle, safe_axis);
}

Common::Quaternion GetRotationFromGyroscope(const Common::Vec3& gyro)
{
  const float turn = gyro.Length();
  if (turn == 0)
    return Common::Quaternion::Identity();
  return Common::Quaternion::Rotate(turn, gyro / turn);
}

Common::Matrix33 GetRotationalMatrix(const Common::Vec3& angle)
{
  const Common::Matrix33 about_x = Common::Matrix33::RotateX(angle.x);
  const Common::Matrix33 about_y = Common::Matrix33::RotateY(angle.y);
  const Common::Matrix33 about_z = Common::Matrix33::RotateZ(angle.z);
  return about_z * about_y * about_x;
}

float GetPitch(const Common::Quaternion& world_rotation)
{
  const Common::Vec3 up = DeviceUp(world_rotation);
  return std::atan2(up.y, Common::Vec2(up.x, up.z).Length());
}

float GetRoll(const Common::Quaternion& world_rotation)
{
  const Common::Vec3 up = DeviceUp(world_rotation);
  return std::atan2(up.x, up.z);
}

float GetYaw(const Common::Quaternion& world_rotation)
{
  const Common::Vec3 forward = world_rotation.Inverted() * Common::Vec3{0, 1, 0};
  return std::atan2(forward.x, forward.y);
}

}  // namespace WiimoteEmu
