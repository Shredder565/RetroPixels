// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/WiimoteEmuMotionControl.h"

#include <QGroupBox>
#include <QHBoxLayout>

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/InputConfig.h"

WiimoteEmuMotionControl::WiimoteEmuMotionControl(MappingWindow* window) : MappingWidget(window)
{
  BuildGroups();
}

void WiimoteEmuMotionControl::BuildGroups()
{
  using WiimoteEmu::WiimoteGroup;

  // The boxes take their titles from the groups themselves.
  static constexpr WiimoteGroup simulated[] = {WiimoteGroup::Shake, WiimoteGroup::Point,
                                               WiimoteGroup::Tilt, WiimoteGroup::Swing};

  auto* const row = new QHBoxLayout();
  for (const WiimoteGroup group : simulated)
    row->addWidget(CreateGroupBox(Wiimote::GetWiimoteGroup(GetPort(), group)));
  setLayout(row);
}

InputConfig* WiimoteEmuMotionControl::GetConfig()
{
  return Wiimote::GetConfig();
}

void WiimoteEmuMotionControl::SaveSettings()
{
  InputConfig* const config = Wiimote::GetConfig();
  config->SaveConfig();
}

void WiimoteEmuMotionControl::LoadSettings()
{
  Wiimote::LoadConfig();
}
