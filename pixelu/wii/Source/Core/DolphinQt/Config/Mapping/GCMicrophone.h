// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Mapping tab for the GameCube microphone in a memory card slot.
class GCMicrophone final : public MappingWidget
{
  Q_OBJECT
public:
  explicit GCMicrophone(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
