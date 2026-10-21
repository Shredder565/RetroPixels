// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/ToolTipControls/BalloonTip.h"

#include <algorithm>
#include <memory>

#include <QApplication>
#include <QBitmap>
#include <QBrush>
#include <QCursor>
#include <QFont>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QVBoxLayout>

#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/Settings.h"

namespace
{
// The one balloon on screen, if any.
std::unique_ptr<BalloonTip> g_balloon;
// The control the balloon belongs to. The balloon is a top-level window, so
// this is tracked here rather than through QObject parenting.
QWidget* g_owner = nullptr;

// Arrow shape, in pixels. The width is odd so the point is one pixel wide.
constexpr int ARROW_WIDTH = 35;
constexpr float ARROW_HALF_WIDTH = ARROW_WIDTH / 2.0f;
constexpr int ARROW_HEIGHT = (ARROW_WIDTH + 1) / 2;
// Gap between the text and the inside of the outline.
constexpr int TEXT_PADDING = 12;
// Inner corner radius of the box; the outline width is added on top.
constexpr float CORNER_RADIUS = 7.0f;
// Preferred distance of the arrow from the box's near side.
constexpr int ARROW_INSET = 34;

const QString EMPHASIS_OPEN = QStringLiteral("<dolphin_emphasis>");
const QString EMPHASIS_CLOSE = QStringLiteral("</dolphin_emphasis>");

// Deleting the balloon from inside one of its own event handlers would free
// the object under Qt's feet, so the hide is posted to the owner instead.
void HideLater()
{
  QueueOnObject(g_owner, BalloonTip::HideBalloon);
}

QPen ThinFlatPen(const QColor& colour, int width)
{
  // Flat caps keep one-pixel strokes from growing past their end points.
  QPen pen(colour, width);
  pen.setCapStyle(Qt::FlatCap);
  return pen;
}
}  // namespace

void BalloonTip::ShowBalloon(const QString& title, const QString& message,
                             const QPoint& target_arrow_tip_position, QWidget* const parent,
                             const ShowArrow show_arrow, const int border_width)
{
  HideBalloon();
  if (title.isEmpty() && message.isEmpty())
    return;

  g_balloon = std::make_unique<BalloonTip>(PrivateTag{}, title, message, parent);
  g_balloon->PlaceAndPaint(target_arrow_tip_position, show_arrow, border_width);
}

void BalloonTip::HideBalloon()
{
  g_owner = nullptr;
  if (!g_balloon)
    return;
  g_balloon->hide();
  g_balloon.reset();
}

bool BalloonTip::IsCursorInsideWidgetBoundingBox(const QWidget& widget)
{
  return widget.rect().contains(widget.mapFromGlobal(QCursor::pos()));
}

bool BalloonTip::IsCursorOnBalloonTip()
{
  if (!g_balloon)
    return false;
  return QApplication::widgetAt(QCursor::pos()) == g_balloon.get();
}

bool BalloonTip::IsWidgetBalloonTipActive(const QWidget& widget)
{
  return g_owner == &widget;
}

BalloonTip::BalloonTip(PrivateTag, const QString& title, QString message, QWidget* const parent)
    : QWidget(nullptr, Qt::ToolTip)
{
  g_owner = parent;
  setMouseTracking(true);

  QColor background;
  QColor foreground;
  QColor highlight;
  Settings::Instance().GetToolTipStyle(background, foreground, highlight, m_outline,
                                       parent->palette(), palette());

  const QString sheet = QStringLiteral("background-color: #%1; color: #%2;")
                            .arg(background.rgba(), 0, 16)
                            .arg(foreground.rgba(), 0, 16);
  setStyleSheet(sheet);

  // The descriptions mark key phrases with a pseudo tag; turn it into real
  // rich text in the highlight colour.
  const QString highlight_open =
      QStringLiteral("<font color=\"#%1\"><b>").arg(highlight.rgba(), 0, 16);
  message.replace(EMPHASIS_OPEN, highlight_open);
  message.replace(EMPHASIS_CLOSE, QStringLiteral("</b></font>"));

  auto* const column = new QVBoxLayout;
  column->setSizeConstraint(QLayout::SetFixedSize);
  setLayout(column);

  if (!title.isEmpty())
  {
    QLabel* const heading = AddTextLabel(column, title);
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);
  }
  if (!message.isEmpty())
    AddTextLabel(column, message);
}

// Long texts wrap at a third of the screen width.
QLabel* BalloonTip::AddTextLabel(QVBoxLayout* const column, const QString& text)
{
  auto* const label = new QLabel;
  column->addWidget(label);
  label->setText(text);
  label->setTextFormat(Qt::RichText);

  const int widest = label->screen()->availableGeometry().width() / 3;
  label->setMaximumWidth(widest);
  if (label->sizeHint().width() > widest)
  {
    label->setWordWrap(true);
    label->setMinimumWidth(widest);
  }
  return label;
}

void BalloonTip::enterEvent(QEnterEvent* const event)
{
  if (!IsCursorInsideWidgetBoundingBox(*g_owner))
    HideLater();
  QWidget::enterEvent(event);
}

void BalloonTip::mouseMoveEvent(QMouseEvent* const event)
{
  if (!IsCursorInsideWidgetBoundingBox(*g_owner))
    HideLater();
  QWidget::mouseMoveEvent(event);
}

void BalloonTip::leaveEvent(QEvent* const event)
{
  if (QApplication::widgetAt(QCursor::pos()) != g_owner)
    HideLater();
  QWidget::leaveEvent(event);
}

void BalloonTip::paintEvent(QPaintEvent* const event)
{
  {
    QPainter painter(this);
    painter.drawPixmap(rect(), m_bubble);
  }
  QWidget::paintEvent(event);
}

// Sizes the window around the labels, paints the bubble (box plus arrow)
// into m_bubble with a matching window mask, and moves the window so the
// arrow's point lands on arrow_target while the box stays on screen.
void BalloonTip::PlaceAndPaint(const QPoint& arrow_target, const ShowArrow show_arrow,
                               const int border_width)
{
  const float half_border = border_width / 2.0f;

  // Room around the labels for the outline, the rounded corners and an arrow
  // above or below.
  const int corner_room = static_cast<int>(half_border / 4);
  const int side_margin = border_width + corner_room + TEXT_PADDING;
  const int end_margin = side_margin + ARROW_HEIGHT;
  layout()->setContentsMargins(side_margin, end_margin, side_margin, end_margin);

  const QSize window_size = sizeHint();
  const int window_w = window_size.width();
  const int window_h = window_size.height();

  // The box outline runs along the middle of the border stroke.
  const float box_left = half_border;
  const float box_top = half_border + ARROW_HEIGHT;
  const float box_w = static_cast<float>(window_w - border_width);
  const float box_h = static_cast<float>(window_h - border_width - 2 * ARROW_HEIGHT);
  const float box_bottom = box_top + box_h;
  // Qt measures a rounded corner's radius to the outside of the curve.
  const float corner = CORNER_RADIUS + static_cast<float>(border_width);

  // Keep the arrow on the straight part of the edge, or centre it on narrow
  // boxes.
  const int arrow_edge =
      std::min(ARROW_INSET + border_width + static_cast<int>(half_border), (window_w - ARROW_WIDTH) / 2);
  const int arrow_point_from_edge = static_cast<int>(arrow_edge + ARROW_HALF_WIDTH);

  QScreen* screen = QGuiApplication::screenAt(arrow_target);
  if (screen == nullptr)  // target off every screen: use the cursor's
    screen = QGuiApplication::screenAt(QCursor::pos());
  const QRect screen_area = screen->geometry();

  // Balloon above the target (arrow on its bottom edge) when there is room,
  // and extending to the right (arrow near its left end) when that fits.
  const bool above = arrow_target.y() - window_h + ARROW_HEIGHT >= 0;
  const bool extends_right =
      arrow_target.x() + window_w - arrow_point_from_edge < screen_area.width();

  const float sign = above ? 1.0f : -1.0f;
  const float arrow_root_y = above ? box_bottom - half_border : box_top + half_border;
  const float arrow_inner_point_y = arrow_root_y + static_cast<float>(above ? ARROW_HEIGHT : -ARROW_HEIGHT);
  const float stroke_step = static_cast<float>(above ? border_width : -border_width);
  const float arrow_outer_point_y = arrow_inner_point_y + stroke_step;
  const float arrow_left =
      static_cast<float>(extends_right ? arrow_edge : window_w - arrow_edge - ARROW_WIDTH);
  const float arrow_right = arrow_left + ARROW_WIDTH;
  const float arrow_mid = arrow_left + ARROW_HALF_WIDTH;

  QPainterPath box;
  box.addRoundedRect(box_left, box_top, box_w, box_h, corner, corner);

  const QColor fill = palette().color(QPalette::Window);
  m_bubble = QPixmap(window_size);
  QBitmap shape(window_size);
  shape.fill(Qt::color0);
  {
    QPainter bubble(&m_bubble);
    QPainter mask(&shape);

    bubble.setPen(ThinFlatPen(m_outline, border_width));
    bubble.setBrush(fill);
    bubble.drawPath(box);

    mask.setPen(ThinFlatPen(Qt::color1, border_width));
    mask.setBrush(QBrush(Qt::color1));
    mask.drawPath(box);

    if (show_arrow == ShowArrow::Yes)
    {
      // Slanted strokes rasterise unpredictably, so the arrow is built from
      // one-pixel vertical strokes instead: a short outline segment and an
      // interior fill segment per column, mirrored about the point.
      QPainterPath outline;
      QPainterPath inside;

      outline.moveTo(arrow_mid, arrow_inner_point_y);
      outline.lineTo(arrow_mid, arrow_inner_point_y + stroke_step);
      inside.moveTo(arrow_mid, arrow_root_y);
      inside.lineTo(arrow_mid, arrow_inner_point_y);

      for (int column = 1; column <= ARROW_HALF_WIDTH; ++column)
      {
        const float inset = static_cast<float>(column) - 0.5f;
        const float edge_y = arrow_root_y + sign * static_cast<float>(column);
        for (const float x : {arrow_left + inset, arrow_right - inset})
        {
          outline.moveTo(x, edge_y);
          outline.lineTo(x, edge_y + stroke_step);
          inside.moveTo(x, arrow_root_y);
          inside.lineTo(x, edge_y);
        }
      }

      bubble.setPen(ThinFlatPen(m_outline, 1));
      bubble.drawPath(outline);
      bubble.setPen(ThinFlatPen(fill, 1));
      bubble.drawPath(inside);

      mask.setPen(ThinFlatPen(Qt::color1, 1));
      mask.drawPath(outline);
      mask.drawPath(inside);
    }
  }
  setMask(shape);

  // Put the arrow's point on the target (drawn or not), then slide the
  // balloon sideways until it is fully on the screen.
  const int wanted_x = arrow_target.x() - static_cast<int>(arrow_mid);
  const int max_x = screen_area.left() + screen_area.width() - window_w;
  const int x = std::max(screen_area.left(), std::min(max_x, wanted_x));
  // arrow_outer_point_y is the far side of the point pixel; when the arrow
  // hangs below the box, the pixel above it is the point.
  const int below_fix = above ? 0 : 1;
  const int y = static_cast<int>(static_cast<float>(arrow_target.y()) - arrow_outer_point_y -
                                 static_cast<float>(below_fix));

  move(x, y);
  show();
}
