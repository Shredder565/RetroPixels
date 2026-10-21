// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_RETRO_ACHIEVEMENTS
#include "DolphinQt/Achievements/AchievementsWindow.h"

#include <mutex>

#include <rcheevos/include/rc_error.h>

#include <QDialogButtonBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Core/AchievementManager.h"

#include "DolphinQt/Achievements/AchievementHeaderWidget.h"
#include "DolphinQt/Achievements/AchievementLeaderboardWidget.h"
#include "DolphinQt/Achievements/AchievementProgressWidget.h"
#include "DolphinQt/Achievements/AchievementSettingsWidget.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"
#include "DolphinQt/Settings.h"

namespace
{
// Tab order: Settings, Progress, Leaderboards.
constexpr int PROGRESS_TAB = 1;
constexpr int LEADERBOARDS_TAB = 2;
}  // namespace

AchievementsWindow::AchievementsWindow(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Achievements"));

  BuildLayout();
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

  // The manager reports from its own threads.
  m_event_hook = AchievementManager::GetInstance().update_event.Register(
      [this](AchievementManager::UpdatedItems updated_items) {
        QueueOnObject(this, [this, items = std::move(updated_items)] { UpdateData(items); });
      });
  UpdateData(AchievementManager::UpdatedItems{.all = true});

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          [this] { m_settings_widget->UpdateData(RC_OK); });
}

void AchievementsWindow::showEvent(QShowEvent* event)
{
  QDialog::showEvent(event);
  update();
}

void AchievementsWindow::SetGameTabsVisible(bool visible)
{
  m_tab_widget->setTabVisible(PROGRESS_TAB, visible);
  m_tab_widget->setTabVisible(LEADERBOARDS_TAB, visible);
}

void AchievementsWindow::BuildLayout()
{
  const bool game_loaded = AchievementManager::GetInstance().IsGameLoaded();

  m_header_widget = new AchievementHeaderWidget(this);
  m_tab_widget = new QTabWidget();
  m_settings_widget = new AchievementSettingsWidget(m_tab_widget);
  m_progress_widget = new AchievementProgressWidget(m_tab_widget);
  m_leaderboard_widget = new AchievementLeaderboardWidget(m_tab_widget);
  m_tab_widget->addTab(GetWrappedWidget(m_settings_widget), tr("Settings"));
  m_tab_widget->addTab(GetWrappedWidget(m_progress_widget), tr("Progress"));
  m_tab_widget->addTab(GetWrappedWidget(m_leaderboard_widget), tr("Leaderboards"));

  m_button_box = new QDialogButtonBox(QDialogButtonBox::Close);

  auto* const outer = new QVBoxLayout{this};
  outer->addWidget(m_header_widget);
  outer->addWidget(m_tab_widget);
  outer->addWidget(m_button_box);

  adjustSize();

  SetGameTabsVisible(game_loaded);
}

void AchievementsWindow::UpdateData(const AchievementManager::UpdatedItems& updated_items)
{
  m_settings_widget->UpdateData(updated_items.failed_login_code);

  if (updated_items.all)
  {
    m_header_widget->UpdateData();
    m_progress_widget->UpdateData(true);
    m_leaderboard_widget->UpdateData(true);
    // Fresh lists start at the top.
    for (int tab : {PROGRESS_TAB, LEADERBOARDS_TAB})
      static_cast<QScrollArea*>(m_tab_widget->widget(tab))->verticalScrollBar()->setValue(0);
  }
  else
  {
    const bool achievements_changed =
        updated_items.all_achievements || !updated_items.achievements.empty();
    if (updated_items.player_icon || updated_items.game_icon || updated_items.rich_presence ||
        achievements_changed)
    {
      m_header_widget->UpdateData();
    }

    if (updated_items.all_achievements || updated_items.rich_presence)
      m_progress_widget->UpdateData(false);
    else if (!updated_items.achievements.empty())
      m_progress_widget->UpdateData(updated_items.achievements);

    if (updated_items.all_leaderboards)
      m_leaderboard_widget->UpdateData(false);
    else if (!updated_items.leaderboards.empty())
      m_leaderboard_widget->UpdateData(updated_items.leaderboards);
  }

  {
    auto& manager = AchievementManager::GetInstance();
    std::lock_guard guard{manager.GetLock()};
    const bool game_loaded = manager.IsGameLoaded();
    m_header_widget->setVisible(manager.HasAPIToken());
    SetGameTabsVisible(game_loaded);
  }
  update();
}

void AchievementsWindow::ForceSettingsTab()
{
  m_tab_widget->setCurrentIndex(0);
}

#endif  // USE_RETRO_ACHIEVEMENTS
