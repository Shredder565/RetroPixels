// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ConfigControls/ConfigInteger.h"

ConfigInteger::ConfigInteger(int minimum, int maximum, const Config::Info<int>& setting, int step)
    : ConfigInteger(minimum, maximum, setting, nullptr, step)
{
}

ConfigInteger::ConfigInteger(int minimum, int maximum, const Config::Info<int>& setting,
                             Config::Layer* layer, int step)
    : ConfigControl(setting.GetLocation(), layer), m_setting(setting)
{
  setSingleStep(step);
  setRange(minimum, maximum);
  OnConfigChanged();
  connect(this, &ConfigInteger::valueChanged, this, &ConfigInteger::Update);
}

void ConfigInteger::Update(int value)
{
  SaveValue(m_setting, value);
}

void ConfigInteger::OnConfigChanged()
{
  setValue(ReadValue(m_setting));
}

ConfigIntegerLabel::ConfigIntegerLabel(const QString& text, ConfigInteger* widget)
    : QLabel(text), m_buddy(widget)
{
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, [this] {
    if (!m_buddy.isNull())
      setFont(m_buddy->font());
  });
}
