// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ConfigControls/ConfigRadio.h"

ConfigRadioInt::ConfigRadioInt(const QString& label, const Config::Info<int>& setting, int value,
                               Config::Layer* layer)
    : ConfigControl(label, setting.GetLocation(), layer), m_setting(setting), m_value(value)
{
  setChecked(m_value == ReadValue(m_setting));
  connect(this, &QRadioButton::toggled, this, &ConfigRadioInt::Toggled);
}

void ConfigRadioInt::Toggled(bool)
{
  if (!isChecked())
  {
    emit OnDeselected(m_value);
    return;
  }
  SaveValue(m_setting, m_value);
  emit OnSelected(m_value);
}

void ConfigRadioInt::OnConfigChanged()
{
  setChecked(m_value == ReadValue(m_setting));
}
