// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Hotkey settings tab: hotkeys for the emulated USB devices.
class HotkeyUSBEmu final : public MappingWidget
{
  Q_OBJECT
public:
  explicit HotkeyUSBEmu(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
