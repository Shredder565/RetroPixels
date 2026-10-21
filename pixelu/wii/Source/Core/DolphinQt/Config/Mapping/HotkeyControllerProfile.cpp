// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/HotkeyControllerProfile.h"

#include <array>

#include <QGridLayout>
#include <QGroupBox>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

HotkeyControllerProfile::HotkeyControllerProfile(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void HotkeyControllerProfile::BuildGroups()
{
  static constexpr std::array<HotkeyGroup, 4> profile_groups = {
      HKGP_CONTROLLER_PROFILE_1, HKGP_CONTROLLER_PROFILE_2, HKGP_CONTROLLER_PROFILE_3,
      HKGP_CONTROLLER_PROFILE_4};

  // A two by two grid, filled row by row: remotes 1 and 2 on top, 3 and 4 below.
  auto* const grid = new QGridLayout;
  for (int remote = 0; remote < static_cast<int>(profile_groups.size()); ++remote)
  {
    const auto group = profile_groups[static_cast<std::size_t>(remote)];
    QGroupBox* const box = CreateGroupBox(tr("Wii Remote %1").arg(remote + 1),
                                          HotkeyManagerEmu::GetHotkeyGroup(group));
    grid->addWidget(box, remote / 2, remote % 2);
  }
  setLayout(grid);
}

void HotkeyControllerProfile::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void HotkeyControllerProfile::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* HotkeyControllerProfile::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
