// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/HotkeyStatesOther.h"

#include <utility>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QString>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

HotkeyStatesOther::HotkeyStatesOther(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void HotkeyStatesOther::BuildGroups()
{
  const std::pair<QString, HotkeyGroup> boxes[] = {
      {tr("Select Last State"), HKGP_SELECT_STATE},
      {tr("Load Last State"), HKGP_LOAD_LAST_STATE},
      {tr("Other State Hotkeys"), HKGP_STATE_MISC},
  };

  // The groups sit side by side, left to right.
  auto* const row = new QHBoxLayout;
  for (const auto& [title, group] : boxes)
    row->addWidget(CreateGroupBox(title, HotkeyManagerEmu::GetHotkeyGroup(group)));
  setLayout(row);
}

void HotkeyStatesOther::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void HotkeyStatesOther::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* HotkeyStatesOther::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
