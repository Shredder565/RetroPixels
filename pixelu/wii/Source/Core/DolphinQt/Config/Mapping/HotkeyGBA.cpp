// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/HotkeyGBA.h"

#include <utility>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QString>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

HotkeyGBA::HotkeyGBA(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void HotkeyGBA::BuildGroups()
{
  const std::pair<QString, HotkeyGroup> boxes[] = {
      {tr("Core"), HKGP_GBA_CORE},
      {tr("Volume"), HKGP_GBA_VOLUME},
      {tr("Window Size"), HKGP_GBA_SIZE},
  };

  // The groups sit side by side, left to right.
  auto* const row = new QHBoxLayout;
  for (const auto& [title, group] : boxes)
    row->addWidget(CreateGroupBox(title, HotkeyManagerEmu::GetHotkeyGroup(group)));
  setLayout(row);
}

void HotkeyGBA::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void HotkeyGBA::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* HotkeyGBA::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
