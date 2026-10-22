// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ToolTipControls/ToolTipComboBox.h"

// Middle of the box.
QPoint ToolTipComboBox::GetToolTipPosition() const
{
  const QSize box = size();
  return pos() + QPoint(box.width() / 2, box.height() / 2);
}
