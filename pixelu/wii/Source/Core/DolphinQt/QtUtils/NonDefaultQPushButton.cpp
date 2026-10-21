// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"

NonDefaultQPushButton::NonDefaultQPushButton(const QString& text, QWidget* parent)
    : QPushButton(text, parent)
{
  setAutoDefault(false);
}
