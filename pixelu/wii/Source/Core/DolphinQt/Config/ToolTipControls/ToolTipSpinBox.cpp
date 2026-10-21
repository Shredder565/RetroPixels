// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ToolTipControls/ToolTipSpinBox.h"

// Middle of the field.
QPoint ToolTipSpinBox::GetToolTipPosition() const
{
  const QSize field = size();
  return pos() + QPoint(field.width() / 2, field.height() / 2);
}
