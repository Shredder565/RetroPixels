// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Free look controller tab with the move, speed, field of view and other groups.
class FreeLookGeneral final : public MappingWidget
{
  Q_OBJECT
public:
  explicit FreeLookGeneral(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
