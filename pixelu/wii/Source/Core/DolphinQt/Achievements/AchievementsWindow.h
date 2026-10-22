// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS
#include <QDialog>

#include "Common/HookableEvent.h"
#include "Core/AchievementManager.h"

class AchievementHeaderWidget;
class AchievementLeaderboardWidget;
class AchievementSettingsWidget;
class AchievementProgressWidget;
class QDialogButtonBox;
class QTabWidget;
class UpdateCallback;

// RetroAchievements: the player/game header over Settings, Progress and Leaderboards tabs. The
// last two only show while a game is loaded. Updates from the achievement manager are applied
// on the UI thread.
class AchievementsWindow : public QDialog
{
  Q_OBJECT
public:
  explicit AchievementsWindow(QWidget* parent);
  void UpdateData(const AchievementManager::UpdatedItems& updated_items);
  void ForceSettingsTab();

private:
  void BuildLayout();
  void showEvent(QShowEvent* event) override;
  void SetGameTabsVisible(bool visible);

  AchievementHeaderWidget* m_header_widget;
  QTabWidget* m_tab_widget;
  AchievementSettingsWidget* m_settings_widget;
  AchievementProgressWidget* m_progress_widget;
  AchievementLeaderboardWidget* m_leaderboard_widget;
  QDialogButtonBox* m_button_box;

  Common::EventHook m_event_hook;
};

#endif  // USE_RETRO_ACHIEVEMENTS
