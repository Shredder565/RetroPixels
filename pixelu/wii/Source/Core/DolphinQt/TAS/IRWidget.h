// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "Common/CommonTypes.h"

// A rectangle standing for the Wii Remote's IR camera view. Left click/drag aims the pointer;
// right click aims it at the centre.
class IRWidget : public QWidget
{
  Q_OBJECT
public:
  explicit IRWidget(QWidget* parent);

  QSize sizeHint() const override;

  static constexpr u16 IR_MIN_X = 0;
  static constexpr u16 IR_MIN_Y = 0;
  static constexpr u16 IR_MAX_X = 1023;
  static constexpr u16 IR_MAX_Y = 767;

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
  u16 m_aim_x = 0;
  u16 m_aim_y = 0;
  // Set by a right click so that dragging afterwards does not move the re-centred pointer.
  bool m_drag_locked = false;
};
