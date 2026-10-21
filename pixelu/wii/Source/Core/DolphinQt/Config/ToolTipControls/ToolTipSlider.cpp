// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ToolTipControls/ToolTipSlider.h"

#include <QStyle>
#include <QStyleOption>

ToolTipSlider::ToolTipSlider(Qt::Orientation orientation) : ToolTipWidget(orientation)
{
}

// The arrow points at the slider's handle.
QPoint ToolTipSlider::GetToolTipPosition() const
{
  QRect handle(0, 0, 15, 15);
  if (QStyle* const current_style = style())
  {
    QStyleOptionSlider option;
    initStyleOption(&option);
    handle = current_style->subControlRect(QStyle::ComplexControl::CC_Slider, &option,
                                           QStyle::SubControl::SC_SliderHandle, this);
  }
  return pos() + handle.center();
}
