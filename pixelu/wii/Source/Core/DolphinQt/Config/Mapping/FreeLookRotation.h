// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Free look controller tab for incremental (motion input) rotation.
class FreeLookRotation final : public MappingWidget
{
  Q_OBJECT
public:
  explicit FreeLookRotation(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
  void OpenAlternateInputSources();
};
