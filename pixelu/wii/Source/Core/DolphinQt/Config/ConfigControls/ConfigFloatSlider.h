// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QLabel>

#include "DolphinQt/Config/ConfigControls/ConfigControl.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipSlider.h"

#include "Common/Config/ConfigInfo.h"

// A slider for a float setting. Internally the QSlider counts steps from
// the minimum (0 .. range/step), so its raw int value and range mean
// nothing to callers; use GetValue().
class ConfigFloatSlider final : public ConfigControl<ToolTipSlider>
{
  Q_OBJECT
public:
  ConfigFloatSlider(float minimum, float maximum, const Config::Info<float>& setting, float step,
                    Config::Layer* layer = nullptr);
  void Update(int value);

  // The setting value the slider currently stands for.
  float GetValue() const;

protected:
  void OnConfigChanged() override;

private:
  int StepsFromMinimum(float value) const;
  float ValueAtStep(int step) const;

  const float m_base;
  const float m_step;
  const Config::Info<float> m_setting;
};

// A caption that turns bold together with its ConfigFloatSlider.
class ConfigFloatLabel final : public QLabel
{
public:
  ConfigFloatLabel(const QString& text, ConfigFloatSlider* widget);
};
