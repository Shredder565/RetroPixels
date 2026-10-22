// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

// Help > About: version, build and licence information.
class AboutDialog final : public QDialog
{
  Q_OBJECT
public:
  explicit AboutDialog(QWidget* parent = nullptr);
};
