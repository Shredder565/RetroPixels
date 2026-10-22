// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// "Extension Motion Input" tab: the Nunchuk's accelerometer fed from real motion sensors.
// MappingWindow only shows it while a Nunchuk is attached.
class WiimoteEmuExtensionMotionInput final : public MappingWidget
{
  Q_OBJECT
public:
  explicit WiimoteEmuExtensionMotionInput(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
