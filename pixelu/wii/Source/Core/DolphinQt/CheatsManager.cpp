// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/CheatsManager.h"

#include <QDialogButtonBox>
#include <QVBoxLayout>

#include "Core/CheatSearch.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"

#include "DolphinQt/CheatSearchFactoryWidget.h"
#include "DolphinQt/CheatSearchWidget.h"
#include "DolphinQt/Config/ARCodeWidget.h"
#include "DolphinQt/Config/GeckoCodeWidget.h"
#include "DolphinQt/QtUtils/PartiallyClosableTabWidget.h"
#include "DolphinQt/Settings.h"

#include "VideoCommon/VideoEvents.h"

namespace
{
const QString GEOMETRY_KEY = QStringLiteral("cheatsmanager/geometry");
}

CheatsManager::CheatsManager(Core::System& system, QWidget* parent)
    : QDialog(parent), m_system(system)
{
  setWindowTitle(tr("Cheats Manager"));

  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &CheatsManager::OnEmulationState);

  BuildTabs();
  Connect();

  OnEmulationState(Core::GetState(system));

  QtUtils::AdjustSizeWithinScreen(this);
  restoreGeometry(Settings::GetQSettings().value(GEOMETRY_KEY).toByteArray());
}

CheatsManager::~CheatsManager()
{
  Settings::GetQSettings().setValue(GEOMETRY_KEY, saveGeometry());
}

void CheatsManager::OnEmulationState(Core::State state)
{
  FollowGame(state);

  // Pausing is a good moment to show the values the game left in memory.
  if (state == Core::State::Paused)
    RefreshAllSearches();
}

void CheatsManager::OnFieldEnd()
{
  if (!isVisible())
    return;

  // Only the search the user is looking at is refreshed every field.
  if (auto* const search = qobject_cast<CheatSearchWidget*>(m_tabs->currentWidget()))
    search->UpdateTableVisibleCurrentValues(CheatSearchWidget::UpdateSource::Auto);
}

void CheatsManager::RefreshAllSearches()
{
  const int tab_count = m_tabs->count();
  for (int tab = 0; tab < tab_count; ++tab)
  {
    if (auto* const search = qobject_cast<CheatSearchWidget*>(m_tabs->widget(tab)))
      search->UpdateTableAllCurrentValues(CheatSearchWidget::UpdateSource::Auto);
  }
}

void CheatsManager::showEvent(QShowEvent*)
{
  m_field_end_hook =
      m_system.GetVideoEvents().vi_end_field_event.Register([this] { OnFieldEnd(); });
}

void CheatsManager::hideEvent(QHideEvent*)
{
  m_field_end_hook.reset();
}

void CheatsManager::FollowGame(Core::State state)
{
  // The game is not settled while emulation starts or stops.
  if (state == Core::State::Starting || state == Core::State::Stopping)
    return;

  const bool game_running = state == Core::State::Running || state == Core::State::Paused;
  const SConfig& config = SConfig::GetInstance();
  std::string game_id = game_running ? config.GetGameID() : std::string();
  std::string game_tdb_id = config.GetGameTDBID();
  const u16 revision = config.GetRevision();

  if (game_id == m_game_id && game_tdb_id == m_game_tdb_id && revision == m_revision)
    return;

  m_game_id = std::move(game_id);
  m_game_tdb_id = std::move(game_tdb_id);
  m_revision = revision;

  m_ar_codes->ChangeGame(m_game_id, m_revision);
  m_gecko_codes->ChangeGame(m_game_id, m_game_tdb_id, m_revision);
}

void CheatsManager::BuildTabs()
{
  m_tabs = new PartiallyClosableTabWidget;
  m_buttons = new QDialogButtonBox(QDialogButtonBox::Close);

  // The first three tabs are permanent; only search tabs can be closed.
  const auto add_permanent_tab = [this](QWidget* page, const QString& title) {
    m_tabs->setTabUnclosable(m_tabs->addTab(GetWrappedWidget(page), title));
  };

  m_ar_codes = new ARCodeWidget(m_game_id, m_revision, false);
  add_permanent_tab(m_ar_codes, tr("AR Code"));

  m_gecko_codes = new GeckoCodeWidget(m_game_id, m_game_tdb_id, m_revision, false);
  add_permanent_tab(m_gecko_codes, tr("Gecko Codes"));

  m_new_search = new CheatSearchFactoryWidget();
  add_permanent_tab(m_new_search, tr("Start New Cheat Search"));

  auto* const layout = new QVBoxLayout;
  layout->addWidget(m_tabs);
  layout->addWidget(m_buttons);
  setLayout(layout);
}

void CheatsManager::AddSearchTab(const Cheats::CheatSearchSessionBase& session)
{
  auto* const search = new CheatSearchWidget(m_system, session.Clone());
  const int tab = m_tabs->addTab(search, tr("Cheat Search"));

  connect(search, &CheatSearchWidget::ActionReplayCodeGenerated, m_ar_codes,
          &ARCodeWidget::AddCode);
  connect(search, &CheatSearchWidget::ShowMemory, this, &CheatsManager::ShowMemory);
  connect(search, &CheatSearchWidget::RequestWatch, this, &CheatsManager::RequestWatch);

  m_tabs->setCurrentIndex(tab);
}

void CheatsManager::CloseTab(int index)
{
  if (QWidget* const page = m_tabs->widget(index))
    page->deleteLater();
}

void CheatsManager::Connect()
{
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_new_search, &CheatSearchFactoryWidget::NewSessionCreated, this,
          &CheatsManager::AddSearchTab);
  connect(m_tabs, &QTabWidget::tabCloseRequested, this, &CheatsManager::CloseTab);

  connect(m_ar_codes, &ARCodeWidget::OpenGeneralSettings, this,
          &CheatsManager::OpenGeneralSettings);
  connect(m_gecko_codes, &GeckoCodeWidget::OpenGeneralSettings, this,
          &CheatsManager::OpenGeneralSettings);
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_ar_codes, &ARCodeWidget::OpenAchievementSettings, this,
          &CheatsManager::OpenAchievementSettings);
  connect(m_gecko_codes, &GeckoCodeWidget::OpenAchievementSettings, this,
          &CheatsManager::OpenAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
}
