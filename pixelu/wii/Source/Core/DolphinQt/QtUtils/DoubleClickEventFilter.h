// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QObject>

// Event filter that emits doubleClicked() for every double click on the objects it watches. It
// never consumes the event.
class DoubleClickEventFilter : public QObject
{
  Q_OBJECT
public:
  using QObject::QObject;

signals:
  void doubleClicked();

private:
  bool eventFilter(QObject* object, QEvent* event) override;
};
