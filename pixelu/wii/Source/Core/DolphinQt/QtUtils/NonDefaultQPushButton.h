// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QPushButton>

// A push button that never becomes a dialog's default button on its own (autoDefault is off), so
// buttons are not highlighted for no reason.
class NonDefaultQPushButton : public QPushButton
{
public:
  explicit NonDefaultQPushButton(const QString& text = {}, QWidget* parent = nullptr);
};
