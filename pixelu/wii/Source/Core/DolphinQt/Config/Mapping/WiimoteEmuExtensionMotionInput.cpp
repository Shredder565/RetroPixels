// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Mapping/WiimoteEmuExtensionMotionInput.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>

#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "DolphinQt/Config/ControllerInterface/ControllerInterfaceWindow.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "InputCommon/InputConfig.h"

WiimoteEmuExtensionMotionInput::WiimoteEmuExtensionMotionInput(MappingWindow* window)
    : MappingWidget(window)
{
  BuildGroups();
}

void WiimoteEmuExtensionMotionInput::BuildGroups()
{
  // Everything sits in one "Nunchuk" frame.
  auto* const nunchuk_frame = new QGroupBox(tr("Nunchuk"), this);
  auto* const frame_grid = new QGridLayout();

  auto* const warning = new QLabel(
      tr("These controls are designed to interface directly with motion sensor hardware. They "
         "are not intended for mapping traditional buttons, triggers or axes. You might need to "
         "configure alternate input sources before using these controls."));
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
  frame_grid->addLayout(warning_row, 0, 0, 1, -1);

  auto* const accelerometer =
      Wiimote::GetNunchukGroup(GetPort(), WiimoteEmu::NunchukGroup::IMUAccelerometer);
  frame_grid->addWidget(CreateGroupBox(tr("Accelerometer"), accelerometer), 1, 0);
  nunchuk_frame->setLayout(frame_grid);

  auto* const page = new QHBoxLayout();
  page->addWidget(nunchuk_frame);
  setLayout(page);
}

InputConfig* WiimoteEmuExtensionMotionInput::GetConfig()
{
  return Wiimote::GetConfig();
}

void WiimoteEmuExtensionMotionInput::LoadSettings()
{
  Wiimote::LoadConfig();
}

void WiimoteEmuExtensionMotionInput::SaveSettings()
{
  InputConfig* const config = Wiimote::GetConfig();
  config->SaveConfig();
}
