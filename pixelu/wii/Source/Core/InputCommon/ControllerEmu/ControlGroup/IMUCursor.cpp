// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/IMUCursor.h"

#include <array>
#include <string>
#include <utility>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
// Enabled by default (the Windows build has no touch pointer that it would get in the way of).
IMUCursor::IMUCursor(std::string name_, std::string ui_name_)
    : ControlGroup(std::move(name_), std::move(ui_name_), GroupType::IMUCursor,
                   ControlGroup::DefaultValue::Enabled)
{
  AddInput(Translatability::Translate, _trans("Recenter"));

  // The defaults reach the screen edges in most games, the Wii Menu included.
  AddSetting(&m_yaw_setting,
             // i18n: Rotation about the vertical ("yaw") axis.
             {_trans("Total Yaw"),
              // i18n: Unit suffix, degrees.
              _trans("°"),
              // i18n: Tooltip of a motion-sensor pointing setting.
              _trans("Clamping of rotation about the yaw axis.")},
             25, 0, 360);

  AddSetting(&m_accel_weight_setting,
             {// i18n: How much the accelerometer contributes to the estimated orientation.
              _trans("Accelerometer Influence"),
              // i18n: Unit suffix, percent.
              _trans("%"),
              // i18n: Tooltip of the accelerometer influence setting.
              _trans("Influence of accelerometer data on pitch and roll. Higher values will reduce "
                     "drift at the cost of noise. Consider values between 1% and 3%.")},
             2, 0, 100);
}

ControlState IMUCursor::GetTotalYaw() const
{
  return MathUtil::TAU * m_yaw_setting.GetValue() / 360;
}

ControlState IMUCursor::GetAccelWeight() const
{
  // The setting is a percentage.
  const ControlState percent = m_accel_weight_setting.GetValue();
  return percent / 100;
}

}  // namespace ControllerEmu
