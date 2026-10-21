// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ToolTipControls/ToolTipCheckBox.h"

#include <QStyle>
#include <QStyleOption>

ToolTipCheckBox::ToolTipCheckBox(const QString& label) : ToolTipWidget(label)
{
  SetTitle(label);
}

// The arrow points at the middle of the tick box rather than the whole control.
QPoint ToolTipCheckBox::GetToolTipPosition() const
{
  int indicator_width = 18;
  if (QStyle* const current_style = style())
  {
    QStyleOptionButton option;
    initStyleOption(&option);
    const QRect box =
        current_style->subElementRect(QStyle::SubElement::SE_CheckBoxIndicator, &option, this);
    indicator_width = box.width();
  }
  return pos() + QPoint(indicator_width / 2, height() / 2);
}
