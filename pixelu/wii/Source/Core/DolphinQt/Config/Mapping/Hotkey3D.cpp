// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/Hotkey3D.h"

#include <utility>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QString>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

Hotkey3D::Hotkey3D(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void Hotkey3D::BuildGroups()
{
  const std::pair<QString, HotkeyGroup> boxes[] = {
      // i18n: Stereoscopic 3D
      {tr("3D"), HKGP_3D_TOGGLE},
      // i18n: Stereoscopic 3D
      {tr("3D Depth"), HKGP_3D_DEPTH},
  };

  // The groups sit side by side, left to right.
  auto* const row = new QHBoxLayout;
  for (const auto& [title, group] : boxes)
    row->addWidget(CreateGroupBox(title, HotkeyManagerEmu::GetHotkeyGroup(group)));
  setLayout(row);
}

void Hotkey3D::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void Hotkey3D::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* Hotkey3D::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
