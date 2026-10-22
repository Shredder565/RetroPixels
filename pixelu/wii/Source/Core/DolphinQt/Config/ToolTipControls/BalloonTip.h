// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QColor>
#include <QPixmap>
#include <QWidget>

class QEnterEvent;
class QEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QPoint;
class QString;
class QVBoxLayout;

// A speech-bubble tool tip: a rounded box with a small arrow pointing at the
// control it describes. Only one exists at a time.
class BalloonTip : public QWidget
{
  Q_OBJECT

  struct PrivateTag
  {
  };

public:
  enum class ShowArrow
  {
    Yes,
    No
  };
  static void ShowBalloon(const QString& title, const QString& message,
                          const QPoint& target_arrow_tip_position, QWidget* parent,
                          ShowArrow show_arrow = ShowArrow::Yes, int border_width = 1);
  static void HideBalloon();
  static bool IsCursorInsideWidgetBoundingBox(const QWidget& widget);
  static bool IsCursorOnBalloonTip();
  static bool IsWidgetBalloonTipActive(const QWidget& widget);

  BalloonTip(PrivateTag, const QString& title, QString message, QWidget* parent);

protected:
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

private:
  QLabel* AddTextLabel(QVBoxLayout* column, const QString& text);
  void PlaceAndPaint(const QPoint& arrow_target, ShowArrow show_arrow, int border_width);

  QColor m_outline;
  QPixmap m_bubble;
};
