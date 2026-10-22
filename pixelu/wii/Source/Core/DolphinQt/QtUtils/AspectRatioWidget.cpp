// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/QtUtils/AspectRatioWidget.h"

#include <QBoxLayout>
#include <QResizeEvent>

AspectRatioWidget::AspectRatioWidget(QWidget* widget, float width, float height, QWidget* parent)
    : QWidget(parent), m_ratio_w(width), m_ratio_h(height)
{
  // Three slots: gap, child, gap. Their stretch factors are recomputed on every resize.
  m_box = new QBoxLayout(QBoxLayout::LeftToRight, this);
  m_box->setContentsMargins(0, 0, 0, 0);
  m_box->addItem(new QSpacerItem(0, 0));
  m_box->addWidget(widget);
  m_box->addItem(new QSpacerItem(0, 0));
}

void AspectRatioWidget::resizeEvent(QResizeEvent* event)
{
  const QSize area = event->size();
  const float wanted = m_ratio_w / m_ratio_h;
  const float area_ratio = static_cast<float>(area.width()) / static_cast<float>(area.height());
  const bool too_wide = area_ratio > wanted;

  // The child's share is the length that keeps the ratio along the constrained axis; the two
  // gaps share what is left.
  int child_share;
  int gap_share;
  if (too_wide)
  {
    m_box->setDirection(QBoxLayout::LeftToRight);
    child_share = static_cast<int>(static_cast<float>(height()) * wanted);
    gap_share = static_cast<int>((width() - child_share) / 2 + 0.5);
  }
  else
  {
    m_box->setDirection(QBoxLayout::TopToBottom);
    child_share = static_cast<int>(static_cast<float>(width()) * (m_ratio_h / m_ratio_w));
    gap_share = static_cast<int>((height() - child_share) / 2 + 0.5);
  }

  m_box->setStretch(0, gap_share);
  m_box->setStretch(1, child_share);
  m_box->setStretch(2, gap_share);
}
