// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS
#include <QWidget>

class QLabel;
class QPushButton;

// Shown above code lists while RetroAchievements hardcore mode is on.
class HardcoreWarningWidget : public QWidget
{
  Q_OBJECT
public:
  explicit HardcoreWarningWidget(QWidget* parent);

signals:
  void OpenAchievementSettings();

private:
  void Refresh();

  QPushButton* m_settings_button;
};
#endif  // USE_RETRO_ACHIEVEMENTS
