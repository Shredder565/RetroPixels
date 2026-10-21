// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/FreeLookGeneral.h"

#include <utility>

#include <QGridLayout>
#include <QGroupBox>
#include <QString>

#include "Core/FreeLookManager.h"
#include "InputCommon/InputConfig.h"

FreeLookGeneral::FreeLookGeneral(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void FreeLookGeneral::BuildGroups()
{
  const std::pair<QString, FreeLookGroup> columns[] = {
      {tr("Move"), FreeLookGroup::Move},
      {tr("Speed"), FreeLookGroup::Speed},
      {tr("Field of View"), FreeLookGroup::FieldOfView},
      {tr("Other"), FreeLookGroup::Other},
  };

  // One row of four boxes.
  auto* const grid = new QGridLayout;
  int column = 0;
  for (const auto& [title, group] : columns)
    grid->addWidget(CreateGroupBox(title, FreeLook::GetInputGroup(GetPort(), group)), 0, column++);
  setLayout(grid);
}

InputConfig* FreeLookGeneral::GetConfig()
{
  return FreeLook::GetInputConfig();
}

void FreeLookGeneral::SaveSettings()
{
  InputConfig* const config = FreeLook::GetInputConfig();
  config->SaveConfig();
}

void FreeLookGeneral::LoadSettings()
{
  FreeLook::LoadInputConfig();
}
