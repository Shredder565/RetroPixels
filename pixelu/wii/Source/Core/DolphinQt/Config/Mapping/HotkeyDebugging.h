// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Hotkey settings tab: debugger stepping, program counter and breakpoint hotkeys.
class HotkeyDebugging final : public MappingWidget
{
  Q_OBJECT
public:
  explicit HotkeyDebugging(MappingWindow* window);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups();
};
