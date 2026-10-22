// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/HotkeyStates.h"

#include <utility>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QString>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

HotkeyStates::HotkeyStates(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void HotkeyStates::BuildGroups()
{
  const std::pair<QString, HotkeyGroup> boxes[] = {
      {tr("Save"), HKGP_SAVE_STATE},
      {tr("Load"), HKGP_LOAD_STATE},
  };

  // The groups sit side by side, left to right.
  auto* const row = new QHBoxLayout;
  for (const auto& [title, group] : boxes)
    row->addWidget(CreateGroupBox(title, HotkeyManagerEmu::GetHotkeyGroup(group)));
  setLayout(row);
}

void HotkeyStates::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void HotkeyStates::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* HotkeyStates::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
