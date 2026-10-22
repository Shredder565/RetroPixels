// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QStatusBar>

// A status bar that reports mouse presses on it through the pressed() signal.
class ClickableStatusBar final : public QStatusBar
{
  Q_OBJECT

public:
  explicit ClickableStatusBar(QWidget* parent) : QStatusBar(parent) {}
  ~ClickableStatusBar() override = default;

signals:
  void pressed();

protected:
  void mousePressEvent(QMouseEvent*) override { emit pressed(); }
};
