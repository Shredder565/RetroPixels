// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/IRWidget.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QPoint>
#include <QRect>

#include "Common/CommonTypes.h"

namespace
{
constexpr int BORDER = 1;

// Converts a widget pixel offset to an IR coordinate in 0 .. limit.
u16 PixelToIR(int pixel, int extent, int limit)
{
  return static_cast<u16>(std::clamp((pixel * limit) / extent, 0, limit));
}
}  // namespace

IRWidget::IRWidget(QWidget* parent) : QWidget(parent)
{
  setMouseTracking(false);
  setToolTip(tr("Left click to set the IR value.\n"
                "Right click to re-center it."));

  // Keeps the 4:3 view from being squashed out of shape.
  setMinimumSize(QSize(64, 48));
}

QSize IRWidget::sizeHint() const
{
  return QSize(128, 96);
}

void IRWidget::SetX(u16 x)
{
  m_aim_x = std::min(x, IR_MAX_X);
  update();
}

void IRWidget::SetY(u16 y)
{
  m_aim_y = std::min(y, IR_MAX_Y);
  update();
}

void IRWidget::paintEvent(QPaintEvent*)
{
  const QRect area(BORDER, BORDER, width() - 2 * BORDER, height() - 2 * BORDER);
  const int mid_x = BORDER + area.width() / 2;
  const int mid_y = BORDER + area.height() / 2;

  // Where the pointer is, in pixels (IR y runs bottom to top).
  const int aim_px = BORDER + (m_aim_x * area.width()) / IR_MAX_X;
  const int aim_py = BORDER + area.height() - (m_aim_y * area.height()) / IR_MAX_Y;
  const int dot = ((area.width() + area.height()) / 2) / 30;

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

  painter.setBrush(Qt::white);
  painter.drawRect(BORDER, BORDER, area.width(), area.height());

  painter.drawLine(BORDER, mid_y, BORDER + area.width(), mid_y);
  painter.drawLine(mid_x, BORDER, mid_x, BORDER + area.height());

  painter.drawLine(mid_x, mid_y, aim_px, aim_py);
  painter.setBrush(Qt::blue);
  painter.drawEllipse(aim_px - dot, aim_py - dot, 2 * dot, 2 * dot);
}

void IRWidget::mousePressEvent(QMouseEvent* event)
{
  handleMouseEvent(event);
  m_drag_locked = (event->button() == Qt::RightButton);
}

void IRWidget::mouseMoveEvent(QMouseEvent* event)
{
  if (!m_drag_locked)
    handleMouseEvent(event);
}

void IRWidget::handleMouseEvent(QMouseEvent* event)
{
  const u16 was_x = m_aim_x;
  const u16 was_y = m_aim_y;

  if (event->button() == Qt::RightButton)
  {
    m_aim_x = static_cast<u16>(std::round(IR_MAX_X / 2.));
    m_aim_y = static_cast<u16>(std::round(IR_MAX_Y / 2.));
  }
  else
  {
    const QPoint at = event->pos();
    m_aim_x = PixelToIR(at.x(), width(), IR_MAX_X);
    // Flip so that the top of the widget is the top of the IR view.
    const int from_bottom = IR_MAX_Y - (at.y() * IR_MAX_Y) / height();
    m_aim_y = static_cast<u16>(std::clamp(from_bottom, 0, static_cast<int>(IR_MAX_Y)));
  }

  bool redraw = false;
  if (m_aim_x != was_x)
  {
    emit ChangedX(m_aim_x);
    redraw = true;
  }
  if (m_aim_y != was_y)
  {
    emit ChangedY(m_aim_y);
    redraw = true;
  }
  if (redraw)
    update();
}
