// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
// Triggers with both a digital click and an analog travel (GameCube L/R, Classic Controller).
// The controls are all the digital buttons first, then the analog axes in the same order.
// A digital press beyond the threshold sets the button bit and forces the analog value to full.
class MixedTriggers : public ControlGroup
{
public:
  explicit MixedTriggers(const std::string& name);

  void GetState(u16* digital, const u16* bitmasks, ControlState* analog,
                bool adjusted = true) const;
  void GetState(u16* digital, const u16* bitmasks, ControlState* analog,
                const InputOverrideFunction& override_func, bool adjusted = true) const;

  ControlState GetDeadzone() const;
  ControlState GetThreshold() const;

  size_t GetTriggerCount() const;

private:
  SettingValue<double> m_threshold_setting;
  SettingValue<double> m_deadzone_setting;
};
}  // namespace ControllerEmu
