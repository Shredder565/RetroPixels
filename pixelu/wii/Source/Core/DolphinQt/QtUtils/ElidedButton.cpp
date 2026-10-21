// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/ElidedButton.h"

#include <QStyleOptionButton>
#include <QStylePainter>

ElidedButton::ElidedButton(const QString& text, Qt::TextElideMode elide_mode)
    : QPushButton(text, nullptr), m_mode{elide_mode}
{
}

Qt::TextElideMode ElidedButton::elideMode() const
{
  return m_mode;
}

void ElidedButton::setElideMode(Qt::TextElideMode elide_mode)
{
  if (m_mode != elide_mode)
  {
    m_mode = elide_mode;
    repaint();
  }
}

QSize ElidedButton::sizeHint() const
{
  // The size hint of a button with long text is wide enough to upset layouts, even ones with fixed
  // sizes. Report what an empty button would want instead; the text is elided to fit anyway.
  const QPushButton blank(parentWidget());
  return blank.sizeHint();
}

void ElidedButton::paintEvent(QPaintEvent*)
{
  QStyleOptionButton option;
  initStyleOption(&option);

  const int room = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this).width();
  option.text = fontMetrics().elidedText(text(), m_mode, room);

  QStylePainter painter{this};
  painter.drawControl(QStyle::CE_PushButton, option);
}
