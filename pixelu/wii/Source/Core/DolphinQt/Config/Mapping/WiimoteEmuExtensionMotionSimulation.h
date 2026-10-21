// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// "Extension Motion Simulation" tab: simulated Nunchuk shake, tilt and swing.
// MappingWindow only shows it while a Nunchuk is attached.
class WiimoteEmuExtensionMotionSimulation final : public MappingWidget
{
  Q_OBJECT
public:
  explicit WiimoteEmuExtensionMotionSimulation(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
