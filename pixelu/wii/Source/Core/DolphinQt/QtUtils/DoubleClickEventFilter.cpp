// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/DoubleClickEventFilter.h"

#include <QEvent>

bool DoubleClickEventFilter::eventFilter(QObject*, QEvent* event)
{
  const bool is_double_click = event->type() == QEvent::MouseButtonDblClick;
  if (is_double_click)
    emit doubleClicked();
  return false;
}
