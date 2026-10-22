// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/TASSlider.h"

#include <QMouseEvent>

TASSlider::TASSlider(int default_, QWidget* parent) : QSlider(parent), m_rest_value(default_)
{
}

TASSlider::TASSlider(int default_, Qt::Orientation orientation, QWidget* parent)
    : QSlider(orientation, parent), m_rest_value(default_)
{
}

void TASSlider::mouseReleaseEvent(QMouseEvent* event)
{
  if (event->button() != Qt::RightButton)
  {
    QSlider::mouseReleaseEvent(event);
    return;
  }
  setValue(m_rest_value);
}
