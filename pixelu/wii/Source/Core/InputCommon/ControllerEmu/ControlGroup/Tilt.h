// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
// Tilting the emulated Wii Remote, mapped like a stick. The result is a fraction of half a turn
// (the gate radius is the maximum angle / 180 degrees).
class Tilt : public ReshapableInput
{
public:
  using StateData = ReshapeData;

  explicit Tilt(const std::string& name);

  ReshapeData GetReshapableState(bool adjusted) const final;
  ControlState GetGateRadiusAtAngle(double angle) const final;

  // The gate radius carries the tilt angle, so the default input shape is the unscaled square.
  ControlState GetDefaultInputRadiusAtAngle(double angle) const final;

  StateData GetState() const;

  // Peak rotational velocity in radians per second.
  ControlState GetMaxRotationalVelocity() const;

private:
  Control* GetModifierInput() const override;

  SettingValue<double> m_max_angle_setting;
  SettingValue<double> m_max_rotational_velocity;
};
}  // namespace ControllerEmu
