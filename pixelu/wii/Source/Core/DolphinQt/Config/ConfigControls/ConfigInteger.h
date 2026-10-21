// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QLabel>
#include <QPointer>

#include "DolphinQt/Config/ConfigControls/ConfigControl.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipSpinBox.h"

#include "Common/Config/ConfigInfo.h"

// A spin box bound to an int setting.
class ConfigInteger final : public ConfigControl<ToolTipSpinBox>
{
  Q_OBJECT
public:
  ConfigInteger(int minimum, int maximum, const Config::Info<int>& setting, int step = 1);
  ConfigInteger(int minimum, int maximum, const Config::Info<int>& setting, Config::Layer* layer,
                int step = 1);

  void Update(int value);

protected:
  void OnConfigChanged() override;

private:
  const Config::Info<int> m_setting;
};

// A caption that turns bold together with its ConfigInteger.
class ConfigIntegerLabel final : public QLabel
{
  Q_OBJECT

public:
  ConfigIntegerLabel(const QString& text, ConfigInteger* widget);

private:
  QPointer<ConfigInteger> m_buddy;
};
