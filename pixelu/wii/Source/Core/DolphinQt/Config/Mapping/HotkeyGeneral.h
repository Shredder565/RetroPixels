// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Hotkey settings tab: general emulator hotkeys, volume and emulation speed.
class HotkeyGeneral final : public MappingWidget
{
  Q_OBJECT
public:
  explicit HotkeyGeneral(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
