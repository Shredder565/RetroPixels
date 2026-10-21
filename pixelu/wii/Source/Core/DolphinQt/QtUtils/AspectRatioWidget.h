// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class QBoxLayout;

// Shows one child widget at a fixed aspect ratio, centred between two stretchable gaps (left and
// right when the area is too wide, above and below when it is too tall).
class AspectRatioWidget : public QWidget
{
  Q_OBJECT
public:
  AspectRatioWidget(QWidget* widget, float width, float height, QWidget* parent = nullptr);
  void resizeEvent(QResizeEvent* event) override;

private:
  QBoxLayout* m_box;
  float m_ratio_w;
  float m_ratio_h;
};
