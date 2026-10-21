// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QPushButton>

// A push button that shortens its text with an ellipsis when it does not fit.
class ElidedButton : public QPushButton
{
  Q_OBJECT
public:
  explicit ElidedButton(const QString& text = {}, Qt::TextElideMode elide_mode = Qt::ElideRight);

  Qt::TextElideMode elideMode() const;
  void setElideMode(Qt::TextElideMode elide_mode);

  QSize sizeHint() const final;

private:
  void paintEvent(QPaintEvent* event) final;
  Qt::TextElideMode m_mode;
};
