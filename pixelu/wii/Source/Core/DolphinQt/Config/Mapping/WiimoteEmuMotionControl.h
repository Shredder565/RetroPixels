// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// "Motion Simulation" tab of an emulated Wii Remote: shake, point, tilt and swing.
class WiimoteEmuMotionControl final : public MappingWidget
{
  Q_OBJECT
public:
  explicit WiimoteEmuMotionControl(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
