// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Settings/GeneralPane.h"

#include <array>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include "Core/AchievementManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/System.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipCheckBox.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipComboBox.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipPushButton.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"

#include "UICommon/AutoUpdate.h"
#ifdef USE_DISCORD_PRESENCE
#include "UICommon/DiscordPresence.h"
#endif

namespace
{
// Entries of the update track combo and the setting value each one stands for. The release
// track is stored as "beta", the name used before the rolling-release switch in 2407, so
// builds from that time still update to current releases.
enum UpdateTrackItem : int
{
  TRACK_NONE = 0,
  TRACK_RELEASES = 1,
  TRACK_DEV = 2,
};
constexpr const char* TRACK_NONE_VALUE = "";
constexpr const char* TRACK_RELEASES_VALUE = "beta";
constexpr const char* TRACK_DEV_VALUE = "dev";

// Entries of the fallback region combo, in display order.
constexpr std::array<DiscIO::Region, 4> FALLBACK_REGIONS = {
    DiscIO::Region::NTSC_J, DiscIO::Region::NTSC_U, DiscIO::Region::PAL, DiscIO::Region::NTSC_K};

QString TrackValueForItem(int item)
{
  switch (item)
  {
  case TRACK_NONE:
    return QString::fromStdString(TRACK_NONE_VALUE);
  case TRACK_RELEASES:
    return QString::fromStdString(TRACK_RELEASES_VALUE);
  case TRACK_DEV:
    return QString::fromStdString(TRACK_DEV_VALUE);
  default:
    return QString();
  }
}

int ItemForTrackValue(const std::string& value)
{
  // Unknown values select the release track, the default on release builds.
  if (value == TRACK_NONE_VALUE)
    return TRACK_NONE;
  if (value == TRACK_DEV_VALUE)
    return TRACK_DEV;
  return TRACK_RELEASES;
}

DiscIO::Region RegionForItem(int item)
{
  if (item >= 0 && item < static_cast<int>(FALLBACK_REGIONS.size()))
    return FALLBACK_REGIONS[static_cast<std::size_t>(item)];
  return DiscIO::Region::NTSC_J;
}

int ItemForRegion(DiscIO::Region region)
{
  for (std::size_t item = 0; item < FALLBACK_REGIONS.size(); ++item)
  {
    if (FALLBACK_REGIONS[item] == region)
      return static_cast<int>(item);
  }
  return 0;
}

// A left/top aligned form whose fields grow, nested into |parent|.
QFormLayout* AddForm(QBoxLayout* parent)
{
  auto* const form = new QFormLayout;
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  parent->addLayout(form);
  return form;
}
}  // namespace

GeneralPane::GeneralPane(QWidget* parent) : QWidget(parent)
{
  auto* const page = new QVBoxLayout;
  BuildBasicGroup(page);
  if (AutoUpdateChecker::SystemSupportsAutoUpdates())
    BuildUpdateGroup(page);
  BuildRegionGroup(page);
#if defined(USE_ANALYTICS) && USE_ANALYTICS
  BuildStatisticsGroup(page);
#endif
  page->addStretch(1);
  setLayout(page);

  ReadSettings();
  SetTooltips();
  WireUp();

  Settings& settings = Settings::Instance();
  connect(&settings, &Settings::EmulationStateChanged, this, &GeneralPane::OnEmulationStateChanged);
  connect(&settings, &Settings::ConfigChanged, this, &GeneralPane::ReadSettings);

  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

QVBoxLayout* GeneralPane::AddGroup(QVBoxLayout* page, const QString& title)
{
  auto* const group = new QGroupBox(title);
  auto* const contents = new QVBoxLayout;
  group->setLayout(contents);
  page->addWidget(group);
  return contents;
}

void GeneralPane::BuildBasicGroup(QVBoxLayout* page)
{
  QVBoxLayout* const group = AddGroup(page, tr("Basic Settings"));

  m_dual_core = new ConfigBool(tr("Enable Dual Core (speedhack)"), Config::MAIN_CPU_THREAD);
  group->addWidget(m_dual_core);

  m_cheats = new ConfigBool(tr("Enable Cheats"), Config::MAIN_ENABLE_CHEATS);
  group->addWidget(m_cheats);

  m_preload_game =
      new ConfigBool(tr("Load Whole Game Into Memory"), Config::MAIN_LOAD_GAME_INTO_MEMORY);
  group->addWidget(m_preload_game);
  m_preload_game->SetDescription(
      tr("Loads the running game into memory in the background.<br><br>This may improve "
         "performance with slow or high-latency storage.<br>System memory requirements will be "
         "much higher with this setting enabled.<br><br><dolphin_emphasis>If unsure, leave "
         "this unchecked.</dolphin_emphasis>"));

  m_mismatched_region =
      new ConfigBool(tr("Allow Mismatched Region Settings"), Config::MAIN_OVERRIDE_REGION_SETTINGS);
  group->addWidget(m_mismatched_region);

  m_auto_disc_change =
      new ConfigBool(tr("Change Discs Automatically"), Config::MAIN_AUTO_DISC_CHANGE);
  group->addWidget(m_auto_disc_change);

#ifdef USE_DISCORD_PRESENCE
  m_discord = new ToolTipCheckBox(tr("Show Current Game on Discord"));
  group->addWidget(m_discord);
#endif

  QFormLayout* const form = AddForm(group);

  // Unlimited, then 10% steps up to 200%; the combo index times 0.1 is the stored speed.
  m_speed_limit = new ToolTipComboBox();
  m_speed_limit->addItem(tr("Unlimited"));
  for (int percent = 10; percent <= 200; percent += 10)
  {
    m_speed_limit->addItem(percent == 100 ? tr("%1% (Normal Speed)").arg(percent) :
                                            QStringLiteral("%1%").arg(percent));
  }
  form->addRow(tr("&Speed Limit:"), m_speed_limit);
}

void GeneralPane::BuildUpdateGroup(QVBoxLayout* page)
{
  auto* const group = new QGroupBox(tr("Auto Update Settings"));
  auto* const form = new QFormLayout;
  group->setLayout(form);
  page->addWidget(group);
  form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  m_update_track = new ToolTipComboBox();
  form->addRow(tr("&Auto Update:"), m_update_track);

  m_update_track->addItem(tr("Don't Update"));
  // i18n: Releases is a noun.
  m_update_track->addItem(tr("Releases (every few months)"));
  m_update_track->addItem(tr("Dev (multiple times a day)"));
}

void GeneralPane::BuildRegionGroup(QVBoxLayout* page)
{
  QVBoxLayout* const group = AddGroup(page, tr("Fallback Region"));
  QFormLayout* const form = AddForm(group);

  m_fallback_region = new ToolTipComboBox();
  form->addRow(tr("Fallback Region:"), m_fallback_region);

  for (const QString& region : {tr("NTSC-J"), tr("NTSC-U"), tr("PAL"), tr("NTSC-K")})
    m_fallback_region->addItem(region);
}

#if defined(USE_ANALYTICS) && USE_ANALYTICS
void GeneralPane::BuildStatisticsGroup(QVBoxLayout* page)
{
  QVBoxLayout* const group = AddGroup(page, tr("Usage Statistics Reporting Settings"));

  m_statistics = new ToolTipCheckBox(tr("Enable Usage Statistics Reporting"));
  m_new_identity = new ToolTipPushButton(tr("Generate a New Statistics Identity"));
  group->addWidget(m_statistics);
  group->addWidget(m_new_identity);
}

void GeneralPane::ResetStatisticsIdentity()
{
  DolphinAnalytics& analytics = DolphinAnalytics::Instance();
  analytics.GenerateNewIdentity();
  analytics.ReloadConfig();

  ModalMessageBox notice(this);
  notice.setIcon(QMessageBox::Information);
  notice.setWindowTitle(tr("Identity Generation"));
  notice.setText(tr("New identity generated."));
  notice.exec();
}
#endif

void GeneralPane::WireUp()
{
  Settings& settings = Settings::Instance();

  // The ConfigBool stores the value first; then everyone else hears about it.
  connect(m_cheats, &QCheckBox::toggled, &settings, &Settings::EnableCheatsChanged);
#ifdef USE_DISCORD_PRESENCE
  connect(m_discord, &QCheckBox::toggled, this, &GeneralPane::WriteSettings);
#endif

  if (AutoUpdateChecker::SystemSupportsAutoUpdates())
  {
    connect(m_update_track, &QComboBox::currentIndexChanged, this, &GeneralPane::WriteSettings);
    connect(&settings, &Settings::AutoUpdateTrackChanged, this, &GeneralPane::ReadSettings);
  }

  connect(m_speed_limit, &QComboBox::currentIndexChanged, this, [this] {
    Config::SetBaseOrCurrent(Config::MAIN_EMULATION_SPEED,
                             static_cast<float>(m_speed_limit->currentIndex()) * 0.1f);
  });

  connect(m_fallback_region, &QComboBox::currentIndexChanged, this, &GeneralPane::WriteSettings);
  connect(&settings, &Settings::FallbackRegionChanged, this, &GeneralPane::ReadSettings);

#if defined(USE_ANALYTICS) && USE_ANALYTICS
  connect(&settings, &Settings::AnalyticsToggled, this, &GeneralPane::ReadSettings);
  connect(m_statistics, &QCheckBox::toggled, this, &GeneralPane::WriteSettings);
  connect(m_new_identity, &QPushButton::clicked, this, &GeneralPane::ResetStatisticsIdentity);
#endif
}

void GeneralPane::ReadSettings()
{
  const QSignalBlocker quiet(this);
  Settings& settings = Settings::Instance();

  if (AutoUpdateChecker::SystemSupportsAutoUpdates())
  {
    const int item = ItemForTrackValue(settings.GetAutoUpdateTrack().toStdString());
    SignalBlocking(m_update_track)->setCurrentIndex(item);
  }

#if defined(USE_ANALYTICS) && USE_ANALYTICS
  SignalBlocking(m_statistics)->setChecked(settings.IsAnalyticsEnabled());
#endif

#ifdef USE_DISCORD_PRESENCE
  SignalBlocking(m_discord)->setChecked(Config::Get(Config::MAIN_USE_DISCORD_PRESENCE));
#endif

  // Speeds that don't match an entry (e.g. set from the command line) leave the combo alone.
  const int speed_item = qRound(Config::Get(Config::MAIN_EMULATION_SPEED) * 10);
  if (speed_item < m_speed_limit->count())
    SignalBlocking(m_speed_limit)->setCurrentIndex(speed_item);

  const int region_item = ItemForRegion(settings.GetFallbackRegion());
  SignalBlocking(m_fallback_region)->setCurrentIndex(region_item);
}

void GeneralPane::WriteSettings()
{
  Config::ConfigChangeCallbackGuard batch;
  Settings& settings = Settings::Instance();

  if (AutoUpdateChecker::SystemSupportsAutoUpdates())
    settings.SetAutoUpdateTrack(TrackValueForItem(m_update_track->currentIndex()));

#ifdef USE_DISCORD_PRESENCE
  Discord::SetDiscordPresenceEnabled(m_discord->isChecked());
#endif

#if defined(USE_ANALYTICS) && USE_ANALYTICS
  settings.SetAnalyticsEnabled(m_statistics->isChecked());
  DolphinAnalytics::Instance().ReloadConfig();
#endif

  settings.SetFallbackRegion(RegionForItem(m_fallback_region->currentIndex()));
}

void GeneralPane::OnEmulationStateChanged(Core::State state)
{
  // These can only be changed before a game starts.
  const bool editable = state == Core::State::Uninitialized;

  m_dual_core->setEnabled(editable);
  m_cheats->setEnabled(editable);
  m_preload_game->setEnabled(editable);
  m_mismatched_region->setEnabled(editable);
#ifdef USE_DISCORD_PRESENCE
  m_discord->setEnabled(editable);
#endif
  m_fallback_region->setEnabled(editable);

  RefreshSpeedLimitTooltip();
}

void GeneralPane::RefreshSpeedLimitTooltip()
{
  const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();

  const QString speed_text =
      tr("Controls how fast emulation runs relative to the original hardware.<br><br>Values "
         "higher than 100% will emulate faster than the original hardware can run, if your "
         "hardware is able to keep up. Values lower than 100% will slow emulation instead. "
         "Unlimited will emulate as fast as your hardware is able to.<br><br><dolphin_emphasis>If "
         "unsure, select 100%.</dolphin_emphasis>");
  if (!hardcore)
  {
    m_speed_limit->SetDescription(speed_text);
    return;
  }

  // Hardcore mode clamps slow-downs, so say so.
  const QString hardcore_note =
      tr("<dolphin_emphasis>When Hardcore Mode is enabled, Speed Limit values less than 100% "
         "will be treated as 100%.</dolphin_emphasis>");
  m_speed_limit->SetDescription(tr("%1<br><br>%2").arg(speed_text).arg(hardcore_note));
}

void GeneralPane::SetTooltips()
{
  m_dual_core->SetDescription(
      tr("Separates CPU and GPU emulation work to separate threads. Reduces single-thread "
         "burden by spreading Dolphin's heaviest load across two cores, which usually improves "
         "performance. However, it can result in glitches and crashes.<br><br>This setting "
         "cannot be changed while emulation is active.<br><br><dolphin_emphasis>If unsure, "
         "leave this unchecked.</dolphin_emphasis>"));

  m_cheats->SetDescription(
      tr("Enables the use of AR and Gecko cheat codes which can be used to modify games' "
         "behavior. These codes can be configured with the Cheats Manager in the Tools "
         "menu.<br><br>This setting cannot be changed while emulation is "
         "active.<br><br><dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>"));

  m_mismatched_region->SetDescription(
      tr("Lets you use languages and other region-related settings that the game may not be "
         "designed for. May cause various crashes and bugs.<br><br>This setting cannot be "
         "changed while emulation is active.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));

  m_auto_disc_change->SetDescription(
      tr("Automatically changes the game disc when requested by games with two discs. This "
         "feature requires the game to be launched in one of the following ways:<br>- From the "
         "game list, with both discs being present in the game list.<br>- With File > Open or "
         "the command line interface, with the paths to both discs being provided.<br>- By "
         "launching an M3U file with File > Open or the command line "
         "interface.<br><br><dolphin_emphasis>If unsure, leave this "
         "unchecked.</dolphin_emphasis>"));

#ifdef USE_DISCORD_PRESENCE
  m_discord->SetDescription(
      tr("Shows which game is active and the duration of your current play session in your "
         "Discord status.<br><br>This setting cannot be changed while emulation is "
         "active.<br><br><dolphin_emphasis>If unsure, leave this checked.</dolphin_emphasis>"));
#endif

  m_speed_limit->SetTitle(tr("Speed Limit"));

  if (AutoUpdateChecker::SystemSupportsAutoUpdates())
  {
    m_update_track->SetTitle(tr("Auto Update"));
    m_update_track->SetDescription(
        tr("Selects which update track Dolphin uses when checking for updates at startup. If a "
           "new update is available, Dolphin will show a list of changes made since your "
           "current version and ask you if you want to update.<br><br>The Dev track has the "
           "latest version of Dolphin which often updates multiple times per day. Select this "
           "track if you want the newest features and fixes.<br><br>The Releases track has an "
           "update every few months. Some reasons you might prefer to use this track:<br>- You "
           "prefer using versions that have had additional testing.<br>- NetPlay requires "
           "players to have the same Dolphin version, and the latest Release version will have "
           "the most players to match with.<br>- You frequently use Dolphin's savestate "
           "system, which doesn't guarantee backward compatibility of savestates between "
           "Dolphin versions. If this applies to you, make sure you make an in-game save before "
           "updating (i.e. save your game in the same way you would on a physical GameCube or "
           "Wii), then load the in-game save after updating Dolphin and before making any new "
           "savestates.<br><br>Selecting \"Don't Update\" will prevent Dolphin from "
           "automatically checking for updates.<br><br><dolphin_emphasis>If unsure, select "
           "Releases.</dolphin_emphasis>"));
  }

  m_fallback_region->SetTitle(tr("Fallback Region"));
  m_fallback_region->SetDescription(
      tr("Sets the region used for titles whose region cannot be determined "
         "automatically.<br><br>This setting cannot be changed while emulation is active."));

#if defined(USE_ANALYTICS) && USE_ANALYTICS
  m_statistics->SetDescription(
      tr("If selected, Dolphin can collect data on its performance, feature usage, emulated "
         "games, and configuration, as well as data on your system's hardware and operating "
         "system.<br><br>No private data is ever collected. This data helps us understand how "
         "people and emulated games use Dolphin and prioritize our efforts. It also helps us "
         "identify rare configurations that are causing bugs, performance and stability "
         "issues."));

  m_new_identity->SetTitle(tr("Generate a New Statistics Identity"));
  m_new_identity->SetDescription(
      tr("Generate a new anonymous ID for your usage statistics. This will cause any future "
         "statistics to be unassociated with your previous statistics."));
#endif
}
