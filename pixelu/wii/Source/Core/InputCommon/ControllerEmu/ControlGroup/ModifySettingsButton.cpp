// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerEmu/ControlGroup/ModifySettingsButton.h"

#include <cstddef>
#include <string>
#include <utility>

#include "InputCommon/ControllerEmu/Control/Control.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace ControllerEmu
{
ModifySettingsButton::ModifySettingsButton(const std::string& button_name) : Buttons(button_name)
{
}

void ModifySettingsButton::AddInput(std::string button_name, bool toggle)
{
  ControlGroup::AddInput(Translatability::Translate, std::move(button_name));
  m_pressed.push_back(false);
  m_active.push_back(false);
  m_is_toggle.push_back(toggle);
}

void ModifySettingsButton::UpdateState()
{
  for (std::size_t i = 0; i != controls.size(); ++i)
  {
    const bool held = controls[i]->GetState<bool>();

    if (!m_is_toggle[i])
    {
      m_active[i] = held;
      continue;
    }

    // A toggle flips once per press (states loaded from savestates do not count as presses).
    if (held && !m_pressed[i])
    {
      const bool now_on = !m_active[i];
      m_active[i] = now_on;
      OSD::AddMessage(controls[i]->ui_name + (now_on ? ": on" : ": off"));
      m_pressed[i] = true;
    }
    if (!held)
      m_pressed[i] = false;
  }
}

const std::vector<bool>& ModifySettingsButton::IsSettingToggled() const
{
  return m_is_toggle;
}

const std::vector<bool>& ModifySettingsButton::GetSettingsModifier() const
{
  return m_active;
}
}  // namespace ControllerEmu
