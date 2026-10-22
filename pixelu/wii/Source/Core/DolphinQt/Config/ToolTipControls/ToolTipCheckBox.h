// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/ToolTipControls/ToolTipWidget.h"

#include <QCheckBox>

class ToolTipCheckBox : public ToolTipWidget<QCheckBox>
{
public:
  explicit ToolTipCheckBox(const QString& label);

private:
  QPoint GetToolTipPosition() const override;
};
