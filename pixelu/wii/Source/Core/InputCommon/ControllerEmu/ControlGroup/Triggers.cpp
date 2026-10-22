// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/Triggers.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace ControllerEmu
{
Triggers::Triggers(const std::string& name_) : ControlGroup(name_, GroupType::Triggers)
{
  AddDeadzoneSetting(&m_deadzone_setting, 50);
}

Triggers::StateData Triggers::GetState() const
{
  return GetState(InputOverrideFunction{});
}

Triggers::StateData Triggers::GetState(const InputOverrideFunction& override_func) const
{
  const ControlState deadzone = m_deadzone_setting.GetValue() / 100;

  StateData result(controls.size());
  for (std::size_t i = 0; i != controls.size(); ++i)
  {
    const Control& trigger = *controls[i];
    ControlState value = ApplyDeadzone(trigger.GetState(), deadzone);
    if (override_func)
    {
      if (const std::optional<ControlState> replaced = override_func(name, trigger.name, value))
        value = *replaced;
    }
    result.data[i] = std::min(value, 1.0);
  }
  return result;
}

}  // namespace ControllerEmu
