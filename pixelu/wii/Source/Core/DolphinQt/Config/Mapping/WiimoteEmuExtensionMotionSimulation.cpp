// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/WiimoteEmuExtensionMotionSimulation.h"

#include <utility>

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QString>

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/InputConfig.h"

WiimoteEmuExtensionMotionSimulation::WiimoteEmuExtensionMotionSimulation(MappingWindow* window)
    : MappingWidget(window)
{
  BuildGroups();
}

void WiimoteEmuExtensionMotionSimulation::BuildGroups()
{
  using WiimoteEmu::NunchukGroup;

  auto* const frame_grid = new QGridLayout();
  auto* const nunchuk_frame = new QGroupBox(tr("Nunchuk"), this);

  // Three boxes in one row inside the Nunchuk frame.
  const std::pair<QString, NunchukGroup> motions[] = {
      {tr("Shake"), NunchukGroup::Shake},
      {tr("Tilt"), NunchukGroup::Tilt},
      {tr("Swing"), NunchukGroup::Swing},
  };
  int column = 0;
  for (const auto& [title, group] : motions)
  {
    auto* const box = CreateGroupBox(title, Wiimote::GetNunchukGroup(GetPort(), group));
    frame_grid->addWidget(box, 0, column++);
  }
  nunchuk_frame->setLayout(frame_grid);

  auto* const page = new QHBoxLayout();
  page->addWidget(nunchuk_frame);
  setLayout(page);
}

void WiimoteEmuExtensionMotionSimulation::SaveSettings()
{
  InputConfig* const config = Wiimote::GetConfig();
  config->SaveConfig();
}

InputConfig* WiimoteEmuExtensionMotionSimulation::GetConfig()
{
  return Wiimote::GetConfig();
}

void WiimoteEmuExtensionMotionSimulation::LoadSettings()
{
  Wiimote::LoadConfig();
}
