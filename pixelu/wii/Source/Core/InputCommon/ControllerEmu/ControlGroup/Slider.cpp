// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/Slider.h"

#include <algorithm>
#include <optional>
#include <string>

#include "Common/Common.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"

namespace ControllerEmu
{
Slider::Slider(const std::string& name_, const std::string& ui_name_)
    : ControlGroup(name_, ui_name_, GroupType::Slider)
{
  AddInput(Translatability::Translate, _trans("Left"));
  AddInput(Translatability::Translate, _trans("Right"));

  AddDeadzoneSetting(&m_deadzone_setting, 50);
}

Slider::Slider(const std::string& name_) : Slider(name_, name_)
{
}

Slider::StateData Slider::GetState() const
{
  return GetState(InputOverrideFunction{});
}

Slider::StateData Slider::GetState(const InputOverrideFunction& override_func) const
{
  const ControlState deadzone = m_deadzone_setting.GetValue() / 100;
  const ControlState position = controls[1]->GetState() - controls[0]->GetState();

  ControlState value = ApplyDeadzone(position, deadzone);
  if (override_func)
  {
    if (const std::optional<ControlState> replaced = override_func(name, X_INPUT_OVERRIDE, value))
      value = *replaced;
  }
  return {std::clamp(value, -1.0, 1.0)};
}

}  // namespace ControllerEmu
