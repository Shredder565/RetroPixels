// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/ToolTipControls/ToolTipWidget.h"

#include <QComboBox>

class ToolTipComboBox : public ToolTipWidget<QComboBox>
{
private:
  QPoint GetToolTipPosition() const override;
};
