// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
// Pointing with a host device's motion sensors.
class IMUCursor : public ControlGroup
{
public:
  IMUCursor(std::string name, std::string ui_name);

  // Yaw movement in radians.
  ControlState GetTotalYaw() const;

  // Weight of the accelerometer in the orientation filter (0..1).
  ControlState GetAccelWeight() const;

private:
  SettingValue<double> m_yaw_setting;
  SettingValue<double> m_accel_weight_setting;
};
}  // namespace ControllerEmu
