// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ToolTipControls/ToolTipRadioButton.h"

#include <QStyle>
#include <QStyleOption>

ToolTipRadioButton::ToolTipRadioButton(const QString& label) : ToolTipWidget(label)
{
  SetTitle(label);
}

// The arrow points at the middle of the round indicator.
QPoint ToolTipRadioButton::GetToolTipPosition() const
{
  int indicator_width = 18;
  if (QStyle* const current_style = style())
  {
    QStyleOptionButton option;
    initStyleOption(&option);
    const QRect dot =
        current_style->subElementRect(QStyle::SubElement::SE_RadioButtonIndicator, &option, this);
    indicator_width = dot.width();
  }
  return pos() + QPoint(indicator_width / 2, height() / 2);
}
