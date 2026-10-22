// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/ToolTipControls/ToolTipWidget.h"

#include <QSlider>

class ToolTipSlider : public ToolTipWidget<QSlider>
{
public:
  explicit ToolTipSlider(Qt::Orientation orientation);

private:
  QPoint GetToolTipPosition() const override;
};
