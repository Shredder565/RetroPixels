// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/Mapping/MappingWidget.h"

// Mapping tab for a standard GameCube controller. The Triforce baseboard variant adds the
// Triforce button group.
class GCPadEmu final : public MappingWidget
{
  Q_OBJECT
public:
  enum class SubType
  {
    StandardController,
    AMBaseboard,
  };

  GCPadEmu(MappingWindow* window, SubType sub_type);

  InputConfig* GetConfig() override;

private:
  void LoadSettings() override;
  void SaveSettings() override;

  void BuildGroups(SubType sub_type);
};

[[nodiscard]] inline auto CreateStandardControllerMappingWidget(MappingWindow* parent)
{
  return new GCPadEmu{parent, GCPadEmu::SubType::StandardController};
}

[[nodiscard]] inline auto CreateAMBaseboardMappingWidget(MappingWindow* parent)
{
  return new GCPadEmu{parent, GCPadEmu::SubType::AMBaseboard};
}
