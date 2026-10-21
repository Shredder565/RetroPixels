// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/ConfigControls/ConfigControl.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipRadioButton.h"

#include "Common/Config/ConfigInfo.h"

// One radio button of a group that together pick the value of an int
// setting; this button stands for m_value.
class ConfigRadioInt final : public ConfigControl<ToolTipRadioButton>
{
  Q_OBJECT
public:
  ConfigRadioInt(const QString& label, const Config::Info<int>& setting, int value,
                 Config::Layer* layer = nullptr);

signals:
  // A change of selection toggles two buttons; these let a listener follow
  // just the one that was picked, or just the one that was dropped.
  void OnSelected(int new_value);
  void OnDeselected(int old_value);

protected:
  void OnConfigChanged() override;

private:
  void Toggled(bool checked);

  const Config::Info<int> m_setting;
  const int m_value;
};
