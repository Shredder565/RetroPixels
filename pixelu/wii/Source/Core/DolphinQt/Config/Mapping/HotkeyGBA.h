// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Hotkey settings tab: Game Boy Advance core, volume and window size hotkeys.
class HotkeyGBA final : public MappingWidget
{
  Q_OBJECT
public:
  explicit HotkeyGBA(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
