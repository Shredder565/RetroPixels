// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

class QLabel;
class QPushButton;

// Shown above the graphics mod list while graphics mods are turned off.
class GraphicsModWarningWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit GraphicsModWarningWidget(QWidget* parent);

signals:
  void GraphicsModEnableSettings();

private:
  void Refresh();

  QLabel* m_message;
  QPushButton* m_settings_button;
};
