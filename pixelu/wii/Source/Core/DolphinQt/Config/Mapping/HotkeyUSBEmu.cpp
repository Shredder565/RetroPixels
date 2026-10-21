// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/HotkeyUSBEmu.h"

#include <utility>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QString>

#include "Core/HotkeyManager.h"
#include "InputCommon/InputConfig.h"

HotkeyUSBEmu::HotkeyUSBEmu(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void HotkeyUSBEmu::BuildGroups()
{
  const std::pair<QString, HotkeyGroup> boxes[] = {
      {tr("USB Device Emulation"), HKGP_USB_EMU},
  };

  // The groups sit side by side, left to right.
  auto* const row = new QHBoxLayout;
  for (const auto& [title, group] : boxes)
    row->addWidget(CreateGroupBox(title, HotkeyManagerEmu::GetHotkeyGroup(group)));
  setLayout(row);
}

void HotkeyUSBEmu::SaveSettings()
{
  InputConfig* const config = HotkeyManagerEmu::GetConfig();
  config->SaveConfig();
}

void HotkeyUSBEmu::LoadSettings()
{
  HotkeyManagerEmu::LoadConfig();
}

InputConfig* HotkeyUSBEmu::GetConfig()
{
  return HotkeyManagerEmu::GetConfig();
}
