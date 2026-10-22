// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Hotkey settings tab: graphics toggles, free look and internal resolution hotkeys.
class HotkeyGraphics final : public MappingWidget
{
  Q_OBJECT
public:
  explicit HotkeyGraphics(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
