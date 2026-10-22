// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"

namespace ControllerEmu
{
// Hotkey-style buttons that switch settings on and off, either while held or as toggles.
class ModifySettingsButton : public Buttons
{
public:
  explicit ModifySettingsButton(const std::string& button_name);

  void AddInput(std::string button_name, bool toggle = false);

  void UpdateState();

  // Per button: whether it toggles (true) or acts while held (false).
  const std::vector<bool>& IsSettingToggled() const;
  // Per button: whether its setting is currently switched on.
  const std::vector<bool>& GetSettingsModifier() const;

private:
  // Per button: a toggle press already counted and not yet released.
  std::vector<bool> m_pressed;
  std::vector<bool> m_is_toggle;
  std::vector<bool> m_active;
};
}  // namespace ControllerEmu
