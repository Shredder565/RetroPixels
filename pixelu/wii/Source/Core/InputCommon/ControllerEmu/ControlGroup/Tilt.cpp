// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/Tilt.h"

#include <string>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
namespace
{
enum TiltControl : std::size_t
{
  TILT_FORWARD,
  TILT_BACKWARD,
  TILT_LEFT,
  TILT_RIGHT,
  TILT_MODIFIER,
};
}  // namespace

Tilt::Tilt(const std::string& name_) : ReshapableInput(name_, name_, GroupType::Tilt)
{
  AddInput(Translatability::Translate, _trans("Forward"));
  AddInput(Translatability::Translate, _trans("Backward"));
  AddInput(Translatability::Translate, _trans("Left"));
  AddInput(Translatability::Translate, _trans("Right"));
  AddInput(Translatability::Translate, _trans("Modifier"));

  AddSetting(&m_max_angle_setting,
             {_trans("Angle"),
              // i18n: Unit suffix, degrees.
              _trans("°"),
              // i18n: Tooltip of the Wii Remote tilt angle setting.
              _trans("Maximum tilt angle.")},
             85, 0, 180);

  AddSetting(&m_max_rotational_velocity,
             {_trans("Velocity"),
              // i18n: Unit suffix, hertz (here: turns per second).
              _trans("Hz"),
              // i18n: Tooltip of the Wii Remote tilt speed setting.
              _trans("Peak angular velocity (measured in turns per second).")},
             7, 1, 50);
}

Tilt::ReshapeData Tilt::GetReshapableState(bool adjusted) const
{
  const ControlState y = controls[TILT_FORWARD]->GetState() - controls[TILT_BACKWARD]->GetState();
  const ControlState x = controls[TILT_RIGHT]->GetState() - controls[TILT_LEFT]->GetState();

  if (!adjusted)
    return {x, y};
  return Reshape(x, y, GetModifierInput()->GetState());
}

Tilt::StateData Tilt::GetState() const
{
  return GetReshapableState(true);
}

ControlState Tilt::GetGateRadiusAtAngle(double angle) const
{
  const ControlState half_turns = m_max_angle_setting.GetValue() / 180;
  return SquareStickGate(half_turns).GetRadiusAtAngle(angle);
}

ControlState Tilt::GetDefaultInputRadiusAtAngle(double angle) const
{
  return SquareStickGate(1.0).GetRadiusAtAngle(angle);
}

ControlState Tilt::GetMaxRotationalVelocity() const
{
  return m_max_rotational_velocity.GetValue() * MathUtil::TAU;
}

Control* Tilt::GetModifierInput() const
{
  return controls[TILT_MODIFIER].get();
}

}  // namespace ControllerEmu
