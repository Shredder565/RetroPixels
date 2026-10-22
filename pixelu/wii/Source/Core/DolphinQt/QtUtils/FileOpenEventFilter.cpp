// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/FileOpenEventFilter.h"

#include <QFileOpenEvent>

FileOpenEventFilter::FileOpenEventFilter(QObject* event_source) : QObject(event_source)
{
  event_source->installEventFilter(this);
}

bool FileOpenEventFilter::eventFilter(QObject*, QEvent* event)
{
  if (event->type() != QEvent::FileOpen)
    return false;

  emit fileOpened(static_cast<QFileOpenEvent*>(event)->file());
  return true;
}
