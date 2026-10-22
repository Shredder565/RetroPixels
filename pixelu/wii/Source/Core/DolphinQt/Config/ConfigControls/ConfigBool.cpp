// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"

ConfigBool::ConfigBool(const QString& label, const Config::Info<bool>& setting, bool reverse)
    : ConfigBool(label, setting, nullptr, reverse)
{
}

ConfigBool::ConfigBool(const QString& label, const Config::Info<bool>& setting,
                       Config::Layer* layer, bool reverse)
    : ConfigControl(label, setting.GetLocation(), layer), m_setting(setting), m_inverted(reverse)
{
  setChecked(ReadValue(m_setting) != m_inverted);
  connect(this, &QCheckBox::toggled, this, &ConfigBool::Store);
}

void ConfigBool::Store(bool)
{
  const bool value = isChecked() != m_inverted;
  SaveValue(m_setting, value);
}

void ConfigBool::OnConfigChanged()
{
  setChecked(ReadValue(m_setting) != m_inverted);
}
