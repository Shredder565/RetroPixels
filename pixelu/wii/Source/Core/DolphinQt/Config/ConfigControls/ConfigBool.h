// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/ConfigControls/ConfigControl.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipCheckBox.h"

#include "Common/Config/ConfigInfo.h"

// A check box bound to a bool setting; with reverse set, ticked means false.
class ConfigBool final : public ConfigControl<ToolTipCheckBox>
{
  Q_OBJECT
public:
  ConfigBool(const QString& label, const Config::Info<bool>& setting, bool reverse = false);
  ConfigBool(const QString& label, const Config::Info<bool>& setting, Config::Layer* layer,
             bool reverse = false);

protected:
  void OnConfigChanged() override;

private:
  void Store(bool checked);

  const Config::Info<bool> m_setting;
  const bool m_inverted;
};
