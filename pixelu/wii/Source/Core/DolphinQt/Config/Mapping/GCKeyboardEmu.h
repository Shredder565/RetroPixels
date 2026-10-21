// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Mapping tab for the GameCube keyboard controller (Phantasy Star Online).
class GCKeyboardEmu final : public MappingWidget
{
  Q_OBJECT
public:
  explicit GCKeyboardEmu(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
