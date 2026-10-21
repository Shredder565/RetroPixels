// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/WiimoteEmuMotionControlIMU.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "DolphinQt/Config/ControllerInterface/ControllerInterfaceWindow.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "InputCommon/InputConfig.h"

WiimoteEmuMotionControlIMU::WiimoteEmuMotionControlIMU(MappingWindow* window)
    : MappingWidget(window)
{
  BuildGroups();
}

void WiimoteEmuMotionControlIMU::BuildGroups()
{
  using WiimoteEmu::WiimoteGroup;

  // A warning that these are for real motion sensors, with a button to set up input sources.
  auto* const warning = new QLabel(
      tr("The controls under Accelerometer and Gyroscope are designed to interface directly "
         "with motion sensor hardware. They are not intended for mapping traditional buttons, "
         "triggers or axes. You might need to configure alternate input sources before using "
         "these controls."));
  warning->setWordWrap(true);
  auto* const sources_button = new QPushButton(tr("Alternate Input Sources"));

  auto* const warning_row = new QHBoxLayout();
  warning_row->addWidget(QtUtils::CreateIconWarning(this, QStyle::SP_MessageBoxWarning, warning),
                         1);
  warning_row->addWidget(sources_button);

  connect(sources_button, &QPushButton::clicked, this, [this] {
    ControllerInterfaceWindow sources_window{this};
    sources_window.exec();
  });

  static constexpr WiimoteGroup sensor_groups[] = {
      WiimoteGroup::IMUPoint, WiimoteGroup::IRPassthrough, WiimoteGroup::IMUAccelerometer,
      WiimoteGroup::IMUGyroscope};
  auto* const groups_row = new QHBoxLayout();
  for (const WiimoteGroup group : sensor_groups)
    groups_row->addWidget(CreateGroupBox(Wiimote::GetWiimoteGroup(GetPort(), group)));

  auto* const page = new QVBoxLayout();
  page->addLayout(warning_row);
  page->addLayout(groups_row);
  setLayout(page);
}

void WiimoteEmuMotionControlIMU::LoadSettings()
{
  Wiimote::LoadConfig();
}

void WiimoteEmuMotionControlIMU::SaveSettings()
{
  InputConfig* const config = Wiimote::GetConfig();
  config->SaveConfig();
}

InputConfig* WiimoteEmuMotionControlIMU::GetConfig()
{
  return Wiimote::GetConfig();
}
