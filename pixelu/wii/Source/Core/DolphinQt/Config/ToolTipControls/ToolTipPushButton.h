// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "DolphinQt/Config/ToolTipControls/ToolTipWidget.h"

#include <QPushButton>

#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"

class ToolTipPushButton : public ToolTipWidget<NonDefaultQPushButton>
{
public:
  explicit ToolTipPushButton(const QString& text = {}, QWidget* parent = nullptr);

private:
  QPoint GetToolTipPosition() const override;
};
