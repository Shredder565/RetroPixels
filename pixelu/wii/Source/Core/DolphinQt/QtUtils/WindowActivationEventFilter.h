// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QObject>

// Reports when a watched window is activated or deactivated. Never consumes events.
class WindowActivationEventFilter : public QObject
{
  Q_OBJECT
public:
  using QObject::QObject;

signals:
  void windowActivated();
  void windowDeactivated();

private:
  bool eventFilter(QObject* object, QEvent* event) override;
};
