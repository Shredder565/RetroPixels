// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/HotkeyDebugging.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QString>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

HotkeyDebugging::HotkeyDebugging(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void HotkeyDebugging::BuildGroups()
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
      {tr("Stepping"), HKGP_STEPPING, 0, 0, -1},
      {tr("Program Counter"), HKGP_PC, 0, 1, 1},
      {tr("Breakpoint"), HKGP_BREAKPOINT, 1, 1, 1},
  };

  auto* const grid = new QGridLayout;
  for (const Cell& cell : cells)
  {
    auto* const group = HotkeyManagerEmu::GetHotkeyGroup(cell.group);
    grid->addWidget(CreateGroupBox(cell.title, group), cell.row, cell.column, cell.row_span, 1);
  }
  setLayout(grid);
}

void HotkeyDebugging::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void HotkeyDebugging::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* HotkeyDebugging::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
