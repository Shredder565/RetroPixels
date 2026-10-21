// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/Force.h"

#include <string>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
namespace
{
enum ForceControl : std::size_t
{
  FORCE_UP,
  FORCE_DOWN,
  FORCE_LEFT,
  FORCE_RIGHT,
  FORCE_FORWARD,
  FORCE_BACKWARD,
};
}  // namespace

Force::Force(const std::string& name_) : ReshapableInput(name_, name_, GroupType::Force)
{
  AddInput(Translatability::Translate, _trans("Up"));
  AddInput(Translatability::Translate, _trans("Down"));
  AddInput(Translatability::Translate, _trans("Left"));
  AddInput(Translatability::Translate, _trans("Right"));
  AddInput(Translatability::Translate, _trans("Forward"));
  AddInput(Translatability::Translate, _trans("Backward"));

  AddSetting(&m_distance_setting,
             {_trans("Distance"),
              // i18n: Unit suffix, centimeters.
              _trans("cm"),
              // i18n: Tooltip of a Wii Remote swing setting.
              _trans("Distance of travel from neutral position.")},
             50, 1, 100);

  // The two speeds set how hard the simulated swing accelerates (over a 1 m reference travel).
  // 40 m/s is roughly a golf club head, faster than games really detect.
  AddSetting(&m_speed_setting,
             {_trans("Speed"),
              // i18n: Unit suffix, meters per second.
              _trans("m/s"),
              // i18n: Tooltip of a Wii Remote swing setting.
              _trans("Peak velocity of outward swing movements.")},
             16, 1, 40);

  // A slow return lets the Remote come back without triggering another swing.
  AddSetting(&m_return_speed_setting,
             {_trans("Return Speed"),
              // i18n: Unit suffix, meters per second.
              _trans("m/s"),
              // i18n: Tooltip of a Wii Remote swing setting.
              _trans("Peak velocity of movements to neutral position.")},
             2, 1, 40);

  AddSetting(&m_angle_setting,
             {_trans("Angle"),
              // i18n: Unit suffix, degrees.
              _trans("°"),
              // i18n: Tooltip of a Wii Remote swing setting.
              _trans("Rotation applied at extremities of swing.")},
             90, 1, 180);
}

Force::ReshapeData Force::GetReshapableState(bool adjusted) const
{
  const ControlState y = controls[FORCE_UP]->GetState() - controls[FORCE_DOWN]->GetState();
  const ControlState x = controls[FORCE_RIGHT]->GetState() - controls[FORCE_LEFT]->GetState();

  if (!adjusted)
    return {x, y};
  return Reshape(x, y);
}

Force::StateData Force::GetState(bool adjusted) const
{
  const ReshapeData planar = GetReshapableState(adjusted);
  ControlState depth = controls[FORCE_FORWARD]->GetState() - controls[FORCE_BACKWARD]->GetState();

  // The depth axis gets the same dead zone and is scaled to the swing distance.
  if (adjusted)
    depth = ApplyDeadzone(depth, GetDeadzonePercentage()) * GetMaxDistance();

  return {static_cast<float>(planar.x), static_cast<float>(planar.y), static_cast<float>(depth)};
}

ControlState Force::GetGateRadiusAtAngle(double) const
{
  // A circle with the swing distance as radius.
  return GetMaxDistance();
}

ControlState Force::GetDefaultInputRadiusAtAngle(double) const
{
  return 1.0;
}

ControlState Force::GetSpeed() const
{
  return m_speed_setting.GetValue();
}

ControlState Force::GetReturnSpeed() const
{
  return m_return_speed_setting.GetValue();
}

ControlState Force::GetTwistAngle() const
{
  return m_angle_setting.GetValue() * MathUtil::TAU / 360;
}

ControlState Force::GetMaxDistance() const
{
  return m_distance_setting.GetValue() / 100;
}

Shake::Shake(const std::string& name_, ControlState default_intensity_scale)
    : ControlGroup(name_, name_, GroupType::Shake)
{
  // i18n: A 3D axis, for mapping motion controls.
  AddInput(Translatability::Translate, _trans("X"));
  // i18n: A 3D axis, for mapping motion controls.
  AddInput(Translatability::Translate, _trans("Y"));
  // i18n: A 3D axis, for mapping motion controls.
  AddInput(Translatability::Translate, _trans("Z"));

  AddDeadzoneSetting(&m_deadzone_setting, 50);

  // In centimeters; a negative value starts the shake in the other direction.
  AddSetting(&m_intensity_setting,
             // i18n: How strongly the emulated Wii Remote is shaken.
             {_trans("Intensity"),
              // i18n: Unit suffix, centimeters.
              _trans("cm"),
              // i18n: Tooltip of a Wii Remote shake setting.
              _trans("Total travel distance.")},
             10 * default_intensity_scale, -50, 50);

  AddSetting(&m_frequency_setting,
             // i18n: How many shakes happen per second.
             {_trans("Frequency"),
              // i18n: Unit suffix, hertz.
              _trans("Hz"),
              // i18n: Tooltip of a Wii Remote shake setting.
              _trans("Number of shakes per second.")},
             6, 1, 20);
}

Shake::StateData Shake::GetState(bool adjusted) const
{
  const float x = static_cast<float>(controls[0]->GetState());
  const float y = static_cast<float>(controls[1]->GetState());
  const float z = static_cast<float>(controls[2]->GetState());
  StateData result = {x, y, z};

  // The mapping UI shows unadjusted values.
  if (adjusted)
  {
    for (float& component : result.data)
      component = ApplyDeadzone(component, static_cast<float>(GetDeadzone()));
  }
  return result;
}

ControlState Shake::GetDeadzone() const
{
  return m_deadzone_setting.GetValue() / 100;
}

ControlState Shake::GetIntensity() const
{
  return m_intensity_setting.GetValue() / 100;
}

ControlState Shake::GetFrequency() const
{
  return m_frequency_setting.GetValue();
}

}  // namespace ControllerEmu
