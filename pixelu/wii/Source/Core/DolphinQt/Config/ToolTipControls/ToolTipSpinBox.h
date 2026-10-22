// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/ToolTipControls/ToolTipWidget.h"

#include <QSpinBox>

class ToolTipSpinBox : public ToolTipWidget<QSpinBox>
{
private:
  QPoint GetToolTipPosition() const override;
};
