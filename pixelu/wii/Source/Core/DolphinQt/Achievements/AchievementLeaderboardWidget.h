// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS
#include <QWidget>

#include "Core/AchievementManager.h"

class QGroupBox;
class QGridLayout;

// The Leaderboards tab: one grid row per leaderboard (title and description, then four entries),
// grouped under the buckets rc_client sorts them into.
class AchievementLeaderboardWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit AchievementLeaderboardWidget(QWidget* parent);
  // clean_all rebuilds the grid; otherwise only the entries are refreshed.
  void UpdateData(bool clean_all);
  void UpdateData(const std::set<AchievementManager::AchievementId>& update_ids);
  void UpdateRow(AchievementManager::AchievementId leaderboard_id);

private:
  void BuildGrid();

  QGroupBox* m_common_box;
  QGridLayout* m_common_layout;
  // Grid row of each leaderboard.
  std::map<AchievementManager::AchievementId, int> m_leaderboard_order;
};

#endif  // USE_RETRO_ACHIEVEMENTS
