// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "Common/Matrix.h"
#include "InputCommon/ControllerEmu/StickGate.h"

namespace ControllerEmu
{
// Swinging the emulated Wii Remote: a reshaped up/down/left/right position plus a
// forward/backward axis, with speeds and a twist angle for the motion simulation.
class Force : public ReshapableInput
{
public:
  using StateData = Common::Vec3;

  explicit Force(const std::string& name);

  ReshapeData GetReshapableState(bool adjusted) const final;
  ControlState GetGateRadiusAtAngle(double ang) const final;

  ControlState GetDefaultInputRadiusAtAngle(double angle) const final;

  StateData GetState(bool adjusted = true) const;

  // Velocities in m/s.
  ControlState GetSpeed() const;
  ControlState GetReturnSpeed() const;

  // Twist at the ends of the swing, in radians.
  ControlState GetTwistAngle() const;

  // Swing distance in meters.
  ControlState GetMaxDistance() const;

private:
  SettingValue<double> m_distance_setting;
  SettingValue<double> m_speed_setting;
  SettingValue<double> m_return_speed_setting;
  SettingValue<double> m_angle_setting;
};

// Shaking the emulated Wii Remote along each axis.
class Shake : public ControlGroup
{
public:
  using StateData = Common::Vec3;

  explicit Shake(const std::string& name, ControlState default_intensity_scale = 1);

  StateData GetState(bool adjusted = true) const;

  ControlState GetDeadzone() const;

  // Total travel distance in meters.
  ControlState GetIntensity() const;

  // Shakes per second.
  ControlState GetFrequency() const;

private:
  SettingValue<double> m_deadzone_setting;
  SettingValue<double> m_intensity_setting;
  SettingValue<double> m_frequency_setting;
};

}  // namespace ControllerEmu
