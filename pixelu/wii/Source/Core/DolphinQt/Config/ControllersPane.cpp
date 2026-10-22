// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ControllersPane.h"

#include <QVBoxLayout>

#include "DolphinQt/Config/CommonControllersWidget.h"
#include "DolphinQt/Config/GamecubeControllersWidget.h"
#include "DolphinQt/Config/WiimoteControllersWidget.h"

ControllersPane::ControllersPane()
{
  auto* const column = new QVBoxLayout{this};

  QWidget* const gamecube = new GamecubeControllersWidget(this);
  m_wiimote_controllers = new WiimoteControllersWidget(this);
  QWidget* const shared = new CommonControllersWidget(this);

  for (QWidget* const section : {gamecube, static_cast<QWidget*>(m_wiimote_controllers), shared})
    column->addWidget(section);
  column->addStretch(1);
}
