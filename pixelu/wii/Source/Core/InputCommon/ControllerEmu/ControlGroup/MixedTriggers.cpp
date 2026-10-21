// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/MixedTriggers.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>

#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "InputCommon/ControllerEmu/Control/Control.h"

namespace ControllerEmu
{
namespace
{
// Reads every trigger. With an override function, each trigger's digital state (as 0 or 1)
// and then its analog value are offered to it before they are stored.
void ReadTriggers(const MixedTriggers& group, u16* digital, const u16* bitmasks,
                  ControlState* analog, const InputOverrideFunction* override_func, bool adjusted)
{
  const ControlState threshold = group.GetThreshold();
  ControlState deadzone = group.GetDeadzone();
  // The mapping UI shows the values without a dead zone.
  if (!adjusted)
    deadzone = 0.0;

  const std::size_t count = group.GetTriggerCount();
  for (std::size_t i = 0; i != count; ++i)
  {
    const Control& button = *group.controls[i];
    const Control& axis = *group.controls[count + i];

    const ControlState press = ControlGroup::ApplyDeadzone(button.GetState(), deadzone);
    ControlState travel = ControlGroup::ApplyDeadzone(axis.GetState(), deadzone);

    bool clicked = false;
    if (press >= threshold)
    {
      clicked = true;
      travel = 1.0;
    }

    if (override_func != nullptr)
    {
      const auto& replace = *override_func;
      if (const auto value = replace(group.name, button.name, static_cast<ControlState>(clicked)))
        clicked = ControlStateCast<bool>(*value);
      if (const auto value = replace(group.name, axis.name, travel))
        travel = *value;
    }

    if (clicked)
      *digital |= bitmasks[i];
    analog[i] = std::min(travel, 1.0);
  }
}
}  // namespace

MixedTriggers::MixedTriggers(const std::string& name_)
    : ControlGroup(name_, GroupType::MixedTriggers)
{
  AddDeadzoneSetting(&m_deadzone_setting, 25);

  AddSetting(&m_threshold_setting,
             {_trans("Threshold"),
              // i18n: Unit suffix, percent.
              _trans("%"),
              // i18n: Tooltip of the press threshold of pressure-sensitive buttons.
              _trans("Input strength required for activation.")},
             90, 1, 100);
}

void MixedTriggers::GetState(u16* const digital, const u16* bitmasks, ControlState* analog,
                             bool adjusted) const
{
  ReadTriggers(*this, digital, bitmasks, analog, nullptr, adjusted);
}

void MixedTriggers::GetState(u16* digital, const u16* bitmasks, ControlState* analog,
                             const InputOverrideFunction& override_func, bool adjusted) const
{
  ReadTriggers(*this, digital, bitmasks, analog, override_func ? &override_func : nullptr,
               adjusted);
}

ControlState MixedTriggers::GetDeadzone() const
{
  return m_deadzone_setting.GetValue() / 100;
}

ControlState MixedTriggers::GetThreshold() const
{
  return m_threshold_setting.GetValue() / 100;
}

size_t MixedTriggers::GetTriggerCount() const
{
  return controls.size() / 2;
}

}  // namespace ControllerEmu
