// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS
#include <QWidget>

class QGroupBox;
class QLabel;
class QProgressBar;

// The top of the achievements window: player and game icons, who is playing what, the score,
// overall progress and the game's rich presence text. Hidden while logged out.
class AchievementHeaderWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit AchievementHeaderWidget(QWidget* parent);
  void UpdateData();

private:
  QLabel* m_user_icon;
  QLabel* m_game_icon;
  QLabel* m_name;
  QLabel* m_points;
  QProgressBar* m_game_progress;
  QLabel* m_progress_label;
  QLabel* m_rich_presence;
  QGroupBox* m_header_box;
};

#endif  // USE_RETRO_ACHIEVEMENTS
