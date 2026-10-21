// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/Slider.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>

#include "Common/Common.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"

namespace ControllerEmu
{
namespace
{
// Input names: moving towards the negative end, then towards the positive end.
constexpr std::array<const char*, 2> SLIDER_INPUT_NAMES = {_trans("Left"), _trans("Right")};
}  // namespace

Slider::Slider(const std::string& name_, const std::string& ui_name_)
    : ControlGroup(name_, ui_name_, GroupType::Slider)
{
  for (const char* input_name : SLIDER_INPUT_NAMES)
    AddInput(Translatability::Translate, input_name);

  // Half the travel is ignored by default.
  AddDeadzoneSetting(&m_deadzone_setting, 50);
}

Slider::Slider(const std::string& name_) : Slider(name_, name_)
{
}

Slider::StateData Slider::GetState() const
{
  const InputOverrideFunction no_override;
  return GetState(no_override);
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
