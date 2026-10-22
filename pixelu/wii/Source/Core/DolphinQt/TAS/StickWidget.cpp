// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/TAS/StickWidget.h"

#include <algorithm>
#include <cmath>

#include <QMouseEvent>
#include <QPainter>
#include <QPoint>

#include "Common/CommonTypes.h"

namespace
{
// Gap between the widget's edge and the drawn pad.
constexpr int MARGIN = 1;

u16 Middle(u16 range)
{
  return static_cast<u16>(std::round(range / 2.));
}

// Maps a widget coordinate (0 .. extent) onto 0 .. range, clamped.
u16 ToStickValue(int widget_coord, int extent, u16 range)
{
  const int scaled = (widget_coord * range) / extent;
  return static_cast<u16>(std::clamp(scaled, 0, static_cast<int>(range)));
}
}  // namespace

StickWidget::StickWidget(QWidget* parent, u16 max_x, u16 max_y)
    : QWidget(parent), m_range_x(max_x), m_range_y(max_y)
{
  setMouseTracking(false);
  setToolTip(tr("Left click to set the stick value.\n"
                "Right click to re-center it."));

  // Any smaller and the circle would no longer look round.
  setMinimumSize(QSize(64, 64));
}

QSize StickWidget::sizeHint() const
{
  return QSize(128, 128);
}

void StickWidget::SetX(u16 x)
{
  m_pos_x = std::min(x, m_range_x);
  update();
}

void StickWidget::SetY(u16 y)
{
  m_pos_y = std::min(y, m_range_y);
  update();
}

void StickWidget::paintEvent(QPaintEvent*)
{
  const int side = std::min(width(), height()) - 2 * MARGIN;
  const int half = side / 2;
  const QPoint centre(MARGIN + half, MARGIN + half);

  // Stick position in widget pixels; y grows downwards on screen but upwards on the stick.
  const int dot_x = MARGIN + (m_pos_x * side) / m_range_x;
  const int dot_y = MARGIN + side - (m_pos_y * side) / m_range_y;
  const int dot_radius = side / 30;

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

  // Grey square backdrop with the white stick gate inscribed in it.
  painter.fillRect(MARGIN, MARGIN, side, side, Qt::lightGray);
  painter.setBrush(Qt::white);
  painter.drawEllipse(MARGIN, MARGIN, side, side);

  // Cross hairs through the middle.
  painter.drawLine(MARGIN, centre.y(), MARGIN + side, centre.y());
  painter.drawLine(centre.x(), MARGIN, centre.x(), MARGIN + side);

  // A line from the middle to the stick, ending in a blue dot.
  painter.drawLine(centre.x(), centre.y(), dot_x, dot_y);
  painter.setBrush(Qt::blue);
  painter.drawEllipse(dot_x - dot_radius, dot_y - dot_radius, 2 * dot_radius, 2 * dot_radius);
}

void StickWidget::mousePressEvent(QMouseEvent* event)
{
  handleMouseEvent(event);
  m_drag_locked = (event->button() == Qt::RightButton);
}

void StickWidget::mouseMoveEvent(QMouseEvent* event)
{
  if (m_drag_locked)
    return;
  handleMouseEvent(event);
}

void StickWidget::handleMouseEvent(QMouseEvent* event)
{
  const u16 old_x = m_pos_x;
  const u16 old_y = m_pos_y;

  if (event->button() == Qt::RightButton)
  {
    m_pos_x = Middle(m_range_x);
    m_pos_y = Middle(m_range_y);
  }
  else
  {
    const QPoint at = event->pos();
    m_pos_x = ToStickValue(at.x(), width(), m_range_x);
    m_pos_y = static_cast<u16>(
        std::clamp(m_range_y - (at.y() * m_range_y) / height(), 0, static_cast<int>(m_range_y)));
  }

  const bool moved_x = (m_pos_x != old_x);
  const bool moved_y = (m_pos_y != old_y);
  if (moved_x)
    emit ChangedX(m_pos_x);
  if (moved_y)
    emit ChangedY(m_pos_y);
  if (moved_x || moved_y)
    update();
}
