// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Hotkey settings tab: frame advance and movie (TAS) hotkeys.
class HotkeyTAS final : public MappingWidget
{
  Q_OBJECT
public:
  explicit HotkeyTAS(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
