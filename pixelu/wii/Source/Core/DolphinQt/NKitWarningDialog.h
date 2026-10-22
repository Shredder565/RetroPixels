// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>
#include <QWidget>

// Warns before an NKit image is booted. The user has to tick that they understand before OK
// becomes available, and can opt out of seeing the warning again.
class NKitWarningDialog final : public QDialog
{
  Q_OBJECT

public:
  // True when booting should go ahead: either the warning is switched off, or it was accepted.
  static bool ShowUnlessDisabled(QWidget* parent = nullptr);

private:
  explicit NKitWarningDialog(QWidget* parent = nullptr);
};
