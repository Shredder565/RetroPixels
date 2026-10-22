// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS
#include <QGroupBox>

#include "rcheevos/include/rc_client.h"

class QLabel;
class QProgressBar;
class QWidget;

struct rc_api_achievement_definition_t;

// One achievement on the Progress tab: badge, title, description, points, unlock state and (for
// measured achievements) a progress bar.
class AchievementBox final : public QGroupBox
{
  Q_OBJECT
public:
  explicit AchievementBox(QWidget* parent, const rc_client_achievement_t* achievement);
  void UpdateData();
  void UpdateProgress();

private:
  QLabel* m_badge = nullptr;
  QLabel* m_status = nullptr;
  QProgressBar* m_progress_bar = nullptr;
  QLabel* m_progress_label = nullptr;

  // Owned by rc_client; valid while the game stays loaded.
  const rc_client_achievement_t* m_achievement;
};

#endif  // USE_RETRO_ACHIEVEMENTS
