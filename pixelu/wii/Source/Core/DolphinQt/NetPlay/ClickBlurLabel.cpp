// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/NetPlay/ClickBlurLabel.h"

#include <QChar>
#include <QGraphicsBlurEffect>
#include <QLabel>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QString>
#include <QWidget>

#include "Common/CommonTypes.h"
#include "Common/Random.h"

namespace
{
// Page indices inside the stack; the blurred page is added first so it is the one shown initially.
constexpr int BLURRED_PAGE = 0;
constexpr int READABLE_PAGE = 1;

QChar RandomFrom(char first, int span)
{
  const int offset = Common::Random::GenerateValue<u8>() % span;
  return QChar(first + offset);
}
}  // namespace

ClickBlurLabel::ClickBlurLabel(QWidget* parent)
    : QStackedWidget(parent), m_normal_label(new QLabel(this)), m_blurred_label(new QLabel(this))
{
  setCursor(Qt::PointingHandCursor);

  // The blur is attached once up front: building the effect at click time can stall slow machines
  // for a noticeable moment, so the stack simply flips between two prepared pages.
  auto* const effect = new QGraphicsBlurEffect(m_blurred_label);
  effect->setBlurRadius(7);
  m_blurred_label->setGraphicsEffect(effect);

  // Only as large as the text itself.
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

  addWidget(m_blurred_label);
  addWidget(m_normal_label);
}

void ClickBlurLabel::setText(const QString& text)
{
  if (text == this->text())
    return;

  m_normal_label->setText(text);
  m_blurred_label->setText(GenerateBlurredText(text));
}

void ClickBlurLabel::mousePressEvent(QMouseEvent* event)
{
  setCurrentIndex(currentIndex() == BLURRED_PAGE ? READABLE_PAGE : BLURRED_PAGE);
  QWidget::mousePressEvent(event);
}

QString ClickBlurLabel::GenerateBlurredText(const QString& text)
{
  QString scrambled;
  scrambled.reserve(text.size());
  for (const QChar& ch : text)
  {
    if (ch.isLetter())
      scrambled += RandomFrom(ch.isUpper() ? 'A' : 'a', 26);
    else if (ch.isDigit())
      scrambled += RandomFrom('0', 10);
    else
      scrambled += ch;
  }
  return scrambled;
}
