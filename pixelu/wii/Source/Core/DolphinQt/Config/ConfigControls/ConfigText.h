// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QLineEdit>

#include "DolphinQt/Config/ConfigControls/ConfigControl.h"

#include "Common/Config/ConfigInfo.h"

// A line edit bound to a string setting, saved when editing finishes.
class ConfigText : public ConfigControl<QLineEdit>
{
  Q_OBJECT
public:
  ConfigText(const Config::Info<std::string>& setting);
  ConfigText(const Config::Info<std::string>& setting, Config::Layer* layer);

  void SetTextAndUpdate(const QString& text);

protected:
  void OnConfigChanged() override;

  const Config::Info<std::string> m_setting;

private:
  void StoreText();
};
