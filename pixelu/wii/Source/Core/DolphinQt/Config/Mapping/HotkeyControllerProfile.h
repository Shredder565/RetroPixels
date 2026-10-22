// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Hotkey settings tab: previous/next/reload profile hotkeys for each of the four Wii Remotes.
class HotkeyControllerProfile final : public MappingWidget
{
  Q_OBJECT
public:
  explicit HotkeyControllerProfile(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
