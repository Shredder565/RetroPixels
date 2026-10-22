// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// "Motion Input" tab of an emulated Wii Remote: point, IR passthrough, accelerometer and
// gyroscope inputs fed from real motion sensors.
class WiimoteEmuMotionControlIMU final : public MappingWidget
{
  Q_OBJECT
public:
  explicit WiimoteEmuMotionControlIMU(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
