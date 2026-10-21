// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
// Analog-only triggers, each from 0 to 1.
class Triggers : public ControlGroup
{
public:
  struct StateData
  {
    StateData() = default;
    explicit StateData(std::size_t trigger_count) : data(trigger_count) {}

    std::vector<ControlState> data;
  };

  explicit Triggers(const std::string& name);

  StateData GetState() const;
  StateData GetState(const InputOverrideFunction& override_func) const;

private:
  SettingValue<double> m_deadzone_setting;
};
}  // namespace ControllerEmu
