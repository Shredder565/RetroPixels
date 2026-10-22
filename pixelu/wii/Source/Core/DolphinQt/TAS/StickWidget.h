// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "Common/CommonTypes.h"

// A round pad showing an analog stick position. Left click/drag places the stick; right click
// puts it back in the middle.
class StickWidget : public QWidget
{
  Q_OBJECT
public:
  explicit StickWidget(QWidget* parent, u16 width, u16 height);

  QSize sizeHint() const override;

signals:
  void ChangedX(u16 x);
  void ChangedY(u16 y);

public slots:
  void SetX(u16 x);
  void SetY(u16 y);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void handleMouseEvent(QMouseEvent* event);

private:
  const u16 m_range_x;
  const u16 m_range_y;
  u16 m_pos_x = 0;
  u16 m_pos_y = 0;
  // Set by a right click so that dragging afterwards does not move the re-centred stick.
  bool m_drag_locked = false;
};
