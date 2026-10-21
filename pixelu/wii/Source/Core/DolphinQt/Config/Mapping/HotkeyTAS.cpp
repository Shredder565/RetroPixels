// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/HotkeyTAS.h"

#include <utility>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QString>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

HotkeyTAS::HotkeyTAS(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void HotkeyTAS::BuildGroups()
{
  const std::pair<QString, HotkeyGroup> boxes[] = {
      {tr("Frame Advance"), HKGP_FRAME_ADVANCE},
      {tr("Movie"), HKGP_MOVIE},
  };

  // The groups sit side by side, left to right.
  auto* const row = new QHBoxLayout;
  for (const auto& [title, group] : boxes)
    row->addWidget(CreateGroupBox(title, HotkeyManagerEmu::GetHotkeyGroup(group)));
  setLayout(row);
}

void HotkeyTAS::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void HotkeyTAS::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* HotkeyTAS::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
