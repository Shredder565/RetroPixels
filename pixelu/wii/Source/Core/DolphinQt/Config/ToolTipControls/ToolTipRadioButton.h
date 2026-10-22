// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/ToolTipControls/ToolTipWidget.h"

#include <QRadioButton>

class ToolTipRadioButton : public ToolTipWidget<QRadioButton>
{
public:
  explicit ToolTipRadioButton(const QString& label);

private:
  QPoint GetToolTipPosition() const override;
};
