// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/AnalogStick.h"

#include <optional>
#include <utility>

#include "Common/Common.h"
#include "Common/MathUtil.h"

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
namespace
{
// Control order: the four directions, then the modifier.
enum StickControl : std::size_t
{
  STICK_UP,
  STICK_DOWN,
  STICK_LEFT,
  STICK_RIGHT,
  STICK_MODIFIER,
};
}  // namespace

AnalogStick::AnalogStick(const char* const name_, std::unique_ptr<StickGate>&& stick_gate)
    : AnalogStick(name_, name_, std::move(stick_gate))
{
}

AnalogStick::AnalogStick(const char* const name_, const char* const ui_name_,
                         std::unique_ptr<StickGate>&& stick_gate)
    : ReshapableInput(name_, ui_name_, GroupType::Stick), m_gate(std::move(stick_gate))
{
  for (const char* direction : named_directions)
    AddInput(Translatability::Translate, direction);
  AddInput(Translatability::Translate, _trans("Modifier"));
}

AnalogStick::ReshapeData AnalogStick::GetReshapableState(bool adjusted) const
{
  const ControlState y = controls[STICK_UP]->GetState() - controls[STICK_DOWN]->GetState();
  const ControlState x = controls[STICK_RIGHT]->GetState() - controls[STICK_LEFT]->GetState();

  // The mapping UI shows the raw values.
  if (!adjusted)
    return {x, y};
  return Reshape(x, y, GetModifierInput()->GetState());
}

AnalogStick::StateData AnalogStick::GetState() const
{
  return GetReshapableState(true);
}

AnalogStick::StateData AnalogStick::GetState(const InputOverrideFunction& override_func) const
{
  StateData state = GetState();
  if (!override_func)
    return state;

  if (const std::optional<ControlState> x = override_func(name, X_INPUT_OVERRIDE, state.x))
    state.x = *x;
  if (const std::optional<ControlState> y = override_func(name, Y_INPUT_OVERRIDE, state.y))
    state.y = *y;
  return state;
}

ControlState AnalogStick::GetGateRadiusAtAngle(double ang) const
{
  return m_gate->GetRadiusAtAngle(ang);
}

Control* AnalogStick::GetModifierInput() const
{
  return controls[STICK_MODIFIER].get();
}

OctagonAnalogStick::OctagonAnalogStick(const char* name_, ControlState gate_radius)
    : OctagonAnalogStick(name_, name_, gate_radius)
{
}

OctagonAnalogStick::OctagonAnalogStick(const char* name_, const char* ui_name_,
                                       ControlState gate_radius)
    : AnalogStick(name_, ui_name_, std::make_unique<OctagonStickGate>(1.0))
{
  AddVirtualNotchSetting(&m_notch_width, 45);

  const NumericSettingDetails gate_size{
      _trans("Gate Size"),
      // i18n: Unit suffix, percent.
      _trans("%"),
      // i18n: The stick gate is the plastic shell around a controller's stick that limits how
      // far it moves.
      _trans("Adjusts target radius of simulated stick gate."), nullptr,
      SettingVisibility::Advanced};
  AddSetting(&m_gate_percent, gate_size, gate_radius * 100, 0.01, 100);
}

ControlState OctagonAnalogStick::GetVirtualNotchSize() const
{
  return m_notch_width.GetValue() * MathUtil::TAU / 360;
}

ControlState OctagonAnalogStick::GetGateRadiusAtAngle(double ang) const
{
  return AnalogStick::GetGateRadiusAtAngle(ang) * m_gate_percent.GetValue() / 100;
}

}  // namespace ControllerEmu
