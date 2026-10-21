// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/HotkeyGeneral.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QString>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

HotkeyGeneral::HotkeyGeneral(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void HotkeyGeneral::BuildGroups()
{
  // Title, hotkey group and grid cell of each box. A row span of -1 fills the column.
  struct Cell
  {
    QString title;
    HotkeyGroup group;
    int row;
    int column;
    int row_span;
  };

  const Cell cells[] = {
      {tr("General"), HKGP_GENERAL, 0, 0, -1},
      {tr("Volume"), HKGP_VOLUME, 0, 1, 1},
      {tr("Emulation Speed"), HKGP_SPEED, 1, 1, 1},
  };

  auto* const grid = new QGridLayout;
  for (const Cell& cell : cells)
  {
    auto* const group = HotkeyManagerEmu::GetHotkeyGroup(cell.group);
    grid->addWidget(CreateGroupBox(cell.title, group), cell.row, cell.column, cell.row_span, 1);
  }
  setLayout(grid);
}

void HotkeyGeneral::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void HotkeyGeneral::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* HotkeyGeneral::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
