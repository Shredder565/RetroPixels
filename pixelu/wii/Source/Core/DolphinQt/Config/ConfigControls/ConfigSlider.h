// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <vector>

#include <QLabel>
#include <QPointer>

#include "DolphinQt/Config/ConfigControls/ConfigControl.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipSlider.h"

#include "Common/CommonTypes.h"
#include "Common/Config/ConfigInfo.h"

// A slider bound to an int setting, either directly (minimum..maximum) or
// through a list of allowed values, one per tick.
class ConfigSlider final : public ConfigControl<ToolTipSlider>
{
  Q_OBJECT
public:
  ConfigSlider(int minimum, int maximum, const Config::Info<int>& setting, int tick = 0);
  ConfigSlider(int minimum, int maximum, const Config::Info<int>& setting, Config::Layer* layer,
               int tick = 0);

  // One tick per entry of tick_values; tick i stands for tick_values[i].
  ConfigSlider(std::vector<int> tick_values, const Config::Info<int>& setting,
               Config::Layer* layer);

  void Update(int value);

protected:
  void OnConfigChanged() override;

private:
  const Config::Info<int> m_setting;
  // Empty for a direct slider.
  std::vector<int> m_stops;
};

// A slider bound to a u32 setting, which holds the slider value times scale.
class ConfigSliderU32 final : public ConfigControl<ToolTipSlider>
{
  Q_OBJECT
public:
  ConfigSliderU32(u32 minimum, u32 maximum, const Config::Info<u32>& setting, u32 scale = 1);
  ConfigSliderU32(u32 minimum, u32 maximum, const Config::Info<u32>& setting, Config ::Layer* layer,
                  u32 scale = 1);

  void Update(u32 value);

protected:
  void OnConfigChanged() override;

private:
  const Config::Info<u32> m_setting;
  const u32 m_scale;
};

// A caption that turns bold together with its ConfigSlider.
class ConfigSliderLabel final : public QLabel
{
  Q_OBJECT

public:
  ConfigSliderLabel(const QString& text, ConfigSlider* slider);

private:
  QPointer<ConfigSlider> m_buddy;
};
