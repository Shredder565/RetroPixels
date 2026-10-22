// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Hotkey settings tab: slot selection, last-state and other state hotkeys.
class HotkeyStatesOther final : public MappingWidget
{
  Q_OBJECT
public:
  explicit HotkeyStatesOther(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
