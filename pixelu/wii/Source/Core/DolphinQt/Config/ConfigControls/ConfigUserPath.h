// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QLineEdit>

#include "Common/Config/ConfigInfo.h"

#include "DolphinQt/Config/ConfigControls/ConfigText.h"

// A path field for one of File's user directories (D_*IDX). It shows the
// directory in use when the setting is empty, and applies edits to File's
// user path table as well as to the setting.
class ConfigUserPath final : public ConfigText
{
  Q_OBJECT
public:
  ConfigUserPath(const unsigned int dir_index, const Config::Info<std::string>& setting);
  ConfigUserPath(const unsigned int dir_index, const Config::Info<std::string>& setting,
                 Config::Layer* layer);

protected:
  void OnConfigChanged() override;

private:
  void ShowEffectivePath();
  void ApplyPath();

  const unsigned int m_dir_index;
};
