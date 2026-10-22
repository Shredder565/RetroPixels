// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ConfigControls/ConfigSlider.h"

#include <algorithm>
#include <cassert>
#include <iterator>

#include <QFont>

#include "DolphinQt/Settings.h"

ConfigSlider::ConfigSlider(int minimum, int maximum, const Config::Info<int>& setting, int tick)
    : ConfigSlider(minimum, maximum, setting, nullptr, tick)
{
}

ConfigSlider::ConfigSlider(int minimum, int maximum, const Config::Info<int>& setting,
                           Config::Layer* layer, int tick)
    : ConfigControl(Qt::Horizontal, setting.GetLocation(), layer), m_setting(setting)
{
  setMinimum(minimum);
  setMaximum(maximum);
  setTickInterval(tick);
  OnConfigChanged();
  connect(this, &ConfigSlider::valueChanged, this, &ConfigSlider::Update);
}

ConfigSlider::ConfigSlider(std::vector<int> tick_values, const Config::Info<int>& setting,
                           Config::Layer* layer)
    : ConfigControl(Qt::Horizontal, setting.GetLocation(), layer), m_setting(setting),
      m_stops(std::move(tick_values))
{
  assert(!m_stops.empty());
  setMinimum(0);
  setMaximum(static_cast<int>(m_stops.size()) - 1);
  setPageStep(1);
  setTickPosition(QSlider::TicksBelow);
  OnConfigChanged();
  connect(this, &ConfigSlider::valueChanged, this, &ConfigSlider::Update);
}

void ConfigSlider::Update(int value)
{
  if (m_stops.empty())
  {
    SaveValue(m_setting, value);
    return;
  }
  if (value < 0 || static_cast<size_t>(value) >= m_stops.size())
    return;
  SaveValue(m_setting, m_stops[static_cast<size_t>(value)]);
}

void ConfigSlider::OnConfigChanged()
{
  const int stored = ReadValue(m_setting);
  if (m_stops.empty())
  {
    setValue(stored);
    return;
  }

  // A value that is not one of the stops cannot be shown, so the slider is
  // disabled until the setting holds one again.
  const auto stop = std::find(m_stops.begin(), m_stops.end(), stored);
  const bool known = stop != m_stops.end();
  setEnabled(true);
  if (known)
    setValue(static_cast<int>(std::distance(m_stops.begin(), stop)));
  else
    setEnabled(false);
}

ConfigSliderU32::ConfigSliderU32(u32 minimum, u32 maximum, const Config::Info<u32>& setting,
                                 u32 scale)
    : ConfigSliderU32(minimum, maximum, setting, nullptr, scale)
{
}

ConfigSliderU32::ConfigSliderU32(u32 minimum, u32 maximum, const Config::Info<u32>& setting,
                                 Config::Layer* layer, u32 scale)
    : ConfigControl(Qt::Horizontal, setting.GetLocation(), layer), m_setting(setting),
      m_scale(scale)
{
  setMinimum(static_cast<int>(minimum));
  setMaximum(static_cast<int>(maximum));
  OnConfigChanged();
  connect(this, &ConfigSliderU32::valueChanged, this, &ConfigSliderU32::Update);
}

void ConfigSliderU32::Update(u32 value)
{
  SaveValue(m_setting, static_cast<u32>(value * m_scale));
}

void ConfigSliderU32::OnConfigChanged()
{
  setValue(static_cast<int>(ReadValue(m_setting) / m_scale));
}

ConfigSliderLabel::ConfigSliderLabel(const QString& text, ConfigSlider* slider)
    : QLabel(text), m_buddy(slider)
{
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, [this] {
    if (!m_buddy.isNull())
      setFont(m_buddy->font());
  });
}
