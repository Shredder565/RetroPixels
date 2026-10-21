// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QSlider>

class QMouseEvent;

// A slider that snaps back to its resting value on a right click.
class TASSlider : public QSlider
{
public:
  explicit TASSlider(int default_, QWidget* parent = nullptr);
  explicit TASSlider(int default_, Qt::Orientation orientation, QWidget* parent = nullptr);

protected:
  void mouseReleaseEvent(QMouseEvent* event) override;

private:
  const int m_rest_value;
};
