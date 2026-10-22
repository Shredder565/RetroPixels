// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"

namespace ControllerEmu
{
// A group of digital buttons. GetState() ORs bitmasks[i] into *buttons for each pressed
// control i (bitmasks has one entry per control, in control order).
class Buttons : public ControlGroup
{
public:
  explicit Buttons(const std::string& name_);
  Buttons(std::string ini_name, std::string group_name);

  template <typename C>
  void GetState(C* const buttons, const C* bitmasks) const
  {
    for (std::size_t i = 0; i != controls.size(); ++i)
    {
      if (controls[i]->GetState<bool>())
        *buttons |= bitmasks[i];
    }
  }

  template <typename C>
  void GetState(C* const buttons, const C* bitmasks,
                const InputOverrideFunction& override_func) const
  {
    if (!override_func)
    {
      GetState(buttons, bitmasks);
      return;
    }

    for (std::size_t i = 0; i != controls.size(); ++i)
    {
      const Control& control = *controls[i];
      ControlState state = control.GetState();
      if (const std::optional<ControlState> replaced = override_func(name, control.name, state))
        state = *replaced;
      if (ControlStateCast<bool>(state))
        *buttons |= bitmasks[i];
    }
  }
};
}  // namespace ControllerEmu
