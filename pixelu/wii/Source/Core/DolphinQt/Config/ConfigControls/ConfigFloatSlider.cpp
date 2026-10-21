// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"

#include <cmath>

#include <QPointer>

ConfigFloatSlider::ConfigFloatSlider(float minimum, float maximum,
                                     const Config::Info<float>& setting, float step,
                                     Config::Layer* layer)
    : ConfigControl(Qt::Horizontal, setting.GetLocation(), layer), m_base(minimum), m_step(step),
      m_setting(setting)
{
  const float span = maximum - minimum;
  const int step_count = static_cast<int>(std::round(span / m_step));

  setMinimum(0);
  setMaximum(step_count);
  setTickInterval(static_cast<int>(std::round(span / static_cast<float>(step_count))));
  setValue(StepsFromMinimum(ReadValue(m_setting)));

  connect(this, &ConfigFloatSlider::valueChanged, this, &ConfigFloatSlider::Update);
}

int ConfigFloatSlider::StepsFromMinimum(float value) const
{
  return static_cast<int>(std::round((value - m_base) / m_step));
}

float ConfigFloatSlider::ValueAtStep(int step) const
{
  return m_step * static_cast<float>(step) + m_base;
}

void ConfigFloatSlider::Update(int value)
{
  SaveValue(m_setting, ValueAtStep(value));
}

float ConfigFloatSlider::GetValue() const
{
  return ValueAtStep(value());
}

void ConfigFloatSlider::OnConfigChanged()
{
  setValue(StepsFromMinimum(ReadValue(m_setting)));
}

ConfigFloatLabel::ConfigFloatLabel(const QString& text, ConfigFloatSlider* widget) : QLabel(text)
{
  const QPointer<ConfigFloatSlider> buddy(widget);
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, [this, buddy] {
    if (!buddy.isNull())
      setFont(buddy->font());
  });
}
