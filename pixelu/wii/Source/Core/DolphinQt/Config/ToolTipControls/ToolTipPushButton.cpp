// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ToolTipControls/ToolTipPushButton.h"

ToolTipPushButton::ToolTipPushButton(const QString& text, QWidget* parent)
    : ToolTipWidget(text, parent)
{
}

// Middle of the button.
QPoint ToolTipPushButton::GetToolTipPosition() const
{
  const QSize button = size();
  return pos() + QPoint(button.width() / 2, button.height() / 2);
}
