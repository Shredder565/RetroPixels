// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/HotkeyGraphics.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QString>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

HotkeyGraphics::HotkeyGraphics(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void HotkeyGraphics::BuildGroups()
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
      {tr("Graphics Toggles"), HKGP_GRAPHICS_TOGGLES, 0, 0, -1},
      {tr("FreeLook"), HKGP_FREELOOK, 0, 1, 1},
      {tr("Internal Resolution"), HKGP_IR, 1, 1, 1},
  };

  auto* const grid = new QGridLayout;
  for (const Cell& cell : cells)
  {
    auto* const group = HotkeyManagerEmu::GetHotkeyGroup(cell.group);
    grid->addWidget(CreateGroupBox(cell.title, group), cell.row, cell.column, cell.row_span, 1);
  }
  setLayout(grid);
}

void HotkeyGraphics::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void HotkeyGraphics::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* HotkeyGraphics::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
