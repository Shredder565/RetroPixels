// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/GBAPadEmu.h"

#include <QGridLayout>
#include <QGroupBox>

#include "Core/HW/GBAPad.h"
#include "Core/HW/GBAPadEmu.h"
#include "InputCommon/InputConfig.h"

GBAPadEmu::GBAPadEmu(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void GBAPadEmu::BuildGroups()
{
  // Two full-height columns, each laying its controls out in two sub-columns.
  constexpr int control_columns = 2;
  auto* const grid = new QGridLayout;

  auto* const dpad = Pad::GetGBAGroup(GetPort(), GBAPadGroup::DPad);
  grid->addWidget(CreateControlsBox(tr("D-Pad"), dpad, control_columns), 0, 0, -1, 1);

  auto* const buttons = Pad::GetGBAGroup(GetPort(), GBAPadGroup::Buttons);
  grid->addWidget(CreateControlsBox(tr("Buttons"), buttons, control_columns), 0, 1, -1, 1);

  setLayout(grid);
}

InputConfig* GBAPadEmu::GetConfig()
{
  return Pad::GetGBAConfig();
}

void GBAPadEmu::LoadSettings()
{
  Pad::LoadGBAConfig();
}

void GBAPadEmu::SaveSettings()
{
  InputConfig* const config = Pad::GetGBAConfig();
  config->SaveConfig();
}
