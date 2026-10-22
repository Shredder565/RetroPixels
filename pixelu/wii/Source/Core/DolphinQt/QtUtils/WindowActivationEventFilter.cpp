// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/WindowActivationEventFilter.h"

#include <QEvent>
#include <QObject>

bool WindowActivationEventFilter::eventFilter(QObject*, QEvent* event)
{
  switch (event->type())
  {
  case QEvent::WindowActivate:
    emit windowActivated();
    break;
  case QEvent::WindowDeactivate:
    emit windowDeactivated();
    break;
  default:
    break;
  }
  return false;
}
