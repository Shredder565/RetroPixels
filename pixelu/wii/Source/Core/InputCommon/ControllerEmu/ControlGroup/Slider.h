// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/CoreDevice.h"

namespace ControllerEmu
{
// A one-axis control from -1 (left) to +1 (right).
class Slider : public ControlGroup
{
public:
  struct StateData
  {
    ControlState value{};
  };

  Slider(const std::string& name_, const std::string& ui_name_);
  explicit Slider(const std::string& name_);

  StateData GetState() const;
  StateData GetState(const InputOverrideFunction& override_func) const;

  // Axis name passed to input override functions.
  static constexpr const char* X_INPUT_OVERRIDE = "X";

private:
  SettingValue<double> m_deadzone_setting;
};
}  // namespace ControllerEmu
