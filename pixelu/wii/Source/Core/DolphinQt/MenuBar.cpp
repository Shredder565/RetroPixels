// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/MenuBar.h"

#include <array>
#include <cctype>
#include <future>
#include <initializer_list>
#include <optional>
#include <string_view>

#include <QAction>
#include <QActionGroup>
#include <QDesktopServices>
#include <QDirIterator>
#include <QFileDialog>
#include <QFontDialog>
#include <QInputDialog>
#include <QMap>
#include <QUrl>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"

#include "Core/AchievementManager.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/RSO.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/WiiSave.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "DiscIO/Enums.h"
#include "DiscIO/NANDImporter.h"

#include "DolphinQt/Host.h"
#include "DolphinQt/NANDRepairDialog.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/NonAutodismissibleMenu.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
#include "DolphinQt/QtUtils/QueueOnObject.h"
#endif
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Updater.h"

#include "UICommon/AutoUpdate.h"
#include "UICommon/GameFile.h"

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
#include <rcheevos/include/rc_client_raintegration.h>
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

QPointer<MenuBar> MenuBar::s_menu_bar;

namespace
{
constexpr int STATE_SLOT_COUNT = 10;

Core::System& TheSystem()
{
  return Core::System::GetInstance();
}

void AnnounceSymbolsChanged()
{
  emit Host::GetInstance()->PPCSymbolsChanged();
}

// A checkable entry that mirrors a boolean config setting (or its opposite, for "Disable ..."
// entries of settings that enable something), written with SetBaseOrCurrent.
QAction* AddConfigToggle(QMenu* menu, const QString& text, const Config::Info<bool>& setting,
                         bool inverted = false)
{
  QAction* const action = menu->addAction(text);
  action->setCheckable(true);
  action->setChecked(Config::Get(setting) != inverted);
  QObject::connect(action, &QAction::toggled, action, [&setting, inverted](bool checked) {
    Config::SetBaseOrCurrent(setting, checked != inverted);
  });
  return action;
}

// A checkable entry bound to one of Settings' visibility flags. When `changed` is given, the
// entry also follows changes made elsewhere.
QAction* AddSettingsToggle(QMenu* menu, const QString& text, bool (Settings::*is_set)() const,
                           void (Settings::*set)(bool),
                           void (Settings::*changed)(bool) = nullptr)
{
  Settings* const settings = &Settings::Instance();
  QAction* const action = menu->addAction(text);
  action->setCheckable(true);
  action->setChecked((settings->*is_set)());
  QObject::connect(action, &QAction::toggled, settings, set);
  if (changed != nullptr)
    QObject::connect(settings, changed, action, &QAction::setChecked);
  return action;
}

void OpenWebPage(const char* url)
{
  QDesktopServices::openUrl(QUrl(QString::fromLatin1(url)));
}
}  // namespace

MenuBar::MenuBar(QWidget* parent) : QMenuBar(parent)
{
  s_menu_bar = this;

  BuildFileMenu();
  BuildEmulationMenu();
  BuildMovieMenu();
  BuildOptionsMenu();
  BuildToolsMenu();
  BuildViewMenu();
  BuildJITMenu();
  BuildSymbolsMenu();
  BuildHelpMenu();

  Settings* const settings = &Settings::Instance();
  connect(settings, &Settings::EmulationStateChanged, this,
          [this](Core::State state) { ReflectEmulationState(state); });
  connect(settings, &Settings::ConfigChanged, this, &MenuBar::ReflectConfig);
  connect(Host::GetInstance(), &Host::UpdateDisasmDialog, this,
          [this] { ReflectEmulationState(Core::GetState(TheSystem())); });

  ReflectEmulationState(Core::GetState(TheSystem()));
  connect(settings, &Settings::DebugModeToggled, this, &MenuBar::ReflectDebugMode);

  connect(this, &MenuBar::SelectionChanged, this, &MenuBar::ReflectSelectedGame);
  connect(this, &MenuBar::RecordingStatusChanged, this, &MenuBar::ReflectRecordingStatus);
  connect(this, &MenuBar::ReadOnlyModeChanged, this,
          [this](bool read_only) { m_read_only->setChecked(read_only); });
}

// --- Menu construction -------------------------------------------------------------------------

void MenuBar::BuildFileMenu()
{
  QMenu* const file = addMenu(tr("&File"));

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
  file->addAction(tr("&Open..."), QKeySequence::Open, this, &MenuBar::Open);
#else
  file->addAction(tr("&Open..."), this, &MenuBar::Open, QKeySequence::Open);
#endif
  file->addSeparator();

  m_change_disc = file->addAction(tr("Change &Disc..."), this, &MenuBar::ChangeDisc);
  m_eject_disc = file->addAction(tr("&Eject Disc"), this, &MenuBar::EjectDisc);
  file->addSeparator();

  file->addAction(tr("Open &User Folder"), this, &MenuBar::OpenUserFolder);
  // The config and cache folders get entries of their own only when they are not simply the
  // usual subfolders of the user folder.
  const std::string user_folder = File::GetUserPath(D_USER_IDX);
  if (File::GetUserPath(D_CONFIG_IDX) != user_folder + CONFIG_DIR DIR_SEP)
    file->addAction(tr("Open &Config Folder"), this, &MenuBar::OpenConfigFolder);
  if (File::GetUserPath(D_CACHE_IDX) != user_folder + CACHE_DIR DIR_SEP)
    file->addAction(tr("Open C&ache Folder"), this, &MenuBar::OpenCacheFolder);
  file->addSeparator();

  QAction* const exit = file->addAction(tr("E&xit"), this, &MenuBar::Exit);
  exit->setShortcuts({QKeySequence::Quit, QKeySequence(Qt::ALT | Qt::Key_F4)});
}

void MenuBar::BuildEmulationMenu()
{
  QMenu* const emulation = addMenu(tr("&Emulation"));

  m_play = emulation->addAction(tr("&Play"), this, &MenuBar::Play);
  m_pause = emulation->addAction(tr("&Pause"), this, &MenuBar::Pause);
  m_stop = emulation->addAction(tr("&Stop"), this, &MenuBar::Stop);
  m_reset = emulation->addAction(tr("&Reset"), this, &MenuBar::Reset);
  m_fullscreen = emulation->addAction(tr("Toggle &Fullscreen"), this, &MenuBar::Fullscreen);
  m_frame_advance = emulation->addAction(tr("&Frame Advance"), this, &MenuBar::FrameAdvance);
  m_screenshot = emulation->addAction(tr("Take Screenshot"), this, &MenuBar::Screenshot);
  emulation->addSeparator();

  BuildSaveStateMenus(emulation);
}

void MenuBar::BuildSaveStateMenus(QMenu* emulation)
{
  m_load_state = emulation->addMenu(tr("&Load State"));
  m_load_state->addAction(tr("Load State from File"), this, &MenuBar::StateLoad);
  m_load_state->addAction(tr("Load State from Selected Slot"), this, &MenuBar::StateLoadSlot);
  m_load_from_slot = m_load_state->addMenu(tr("Load State from Slot"));
  m_load_state->addAction(tr("Undo Load State"), this, &MenuBar::StateLoadUndo);
  for (int slot = 1; slot <= STATE_SLOT_COUNT; ++slot)
  {
    // Labels are filled in by RelabelStateSlots.
    QAction* const entry = m_load_from_slot->addAction(QString{});
    connect(entry, &QAction::triggered, this, [this, slot] { emit StateLoadSlotAt(slot); });
  }

  m_save_state = emulation->addMenu(tr("Sa&ve State"));
  m_save_state->addAction(tr("Save State to File"), this, &MenuBar::StateSave);
  m_save_state->addAction(tr("Save State to Selected Slot"), this, &MenuBar::StateSaveSlot);
  m_save_state->addAction(tr("Save State to Oldest Slot"), this, &MenuBar::StateSaveOldest);
  m_save_to_slot = m_save_state->addMenu(tr("Save State to Slot"));
  m_save_state->addAction(tr("Undo Save State"), this, &MenuBar::StateSaveUndo);
  for (int slot = 1; slot <= STATE_SLOT_COUNT; ++slot)
  {
    QAction* const entry = m_save_to_slot->addAction(QString{});
    connect(entry, &QAction::triggered, this, [this, slot] { emit StateSaveSlotAt(slot); });
  }

  m_select_slot = emulation->addMenu(tr("Select State Slot"));
  m_slot_choices = new QActionGroup(this);
  for (int slot = 1; slot <= STATE_SLOT_COUNT; ++slot)
  {
    QAction* const entry = m_select_slot->addAction(QString{});
    entry->setCheckable(true);
    entry->setActionGroup(m_slot_choices);
    if (Settings::Instance().GetStateSlot() == slot)
      entry->setChecked(true);

    connect(entry, &QAction::triggered, this, [this, slot] { emit SetStateSlot(slot); });
    // Follow slot changes from anywhere (hotkeys included).
    connect(this, &MenuBar::SetStateSlot, entry, [entry, slot](int selected) {
      if (selected == slot)
        entry->setChecked(true);
    });
  }

  RelabelStateSlots();

  // The labels show what each slot holds, which can change at any time.
  for (QMenu* const menu : {m_load_state, m_save_state, m_select_slot})
    connect(menu, &QMenu::aboutToShow, this, &MenuBar::RelabelStateSlots);
}

void MenuBar::BuildMovieMenu()
{
  auto* const movie = new QtUtils::NonAutodismissibleMenu(tr("&Movie"), this);
  addMenu(movie);

  m_start_recording =
      movie->addAction(tr("Start Re&cording Input"), this, [this] { emit StartRecording(); });
  m_play_recording =
      movie->addAction(tr("P&lay Input Recording..."), this, [this] { emit PlayRecording(); });
  m_stop_recording =
      movie->addAction(tr("Stop Playing/Recording Input"), this, [this] { emit StopRecording(); });
  m_export_recording =
      movie->addAction(tr("Export Recording..."), this, [this] { emit ExportRecording(); });
  for (QAction* const action :
       {m_start_recording, m_play_recording, m_stop_recording, m_export_recording})
  {
    action->setEnabled(false);
  }

  m_read_only = movie->addAction(tr("&Read-Only Mode"));
  m_read_only->setCheckable(true);
  m_read_only->setChecked(TheSystem().GetMovie().IsReadOnly());
  connect(m_read_only, &QAction::toggled, this,
          [](bool read_only) { TheSystem().GetMovie().SetReadOnly(read_only); });

  movie->addAction(tr("TAS Input"), this, [this] { emit ShowTASInput(); });
  movie->addSeparator();

  AddConfigToggle(movie, tr("Pause at End of Movie"), Config::MAIN_MOVIE_PAUSE_MOVIE);
  m_movie_window = AddConfigToggle(movie, tr("Enable Movie Window"), Config::MAIN_MOVIE_SHOW_OSD);
  movie->addAction(tr("Customize Movie Window"), this, &MenuBar::ConfigureOSD);
  movie->addSeparator();

  AddConfigToggle(movie, tr("Dump Frames"), Config::MAIN_MOVIE_DUMP_FRAMES);
  AddConfigToggle(movie, tr("Dump Audio"), Config::MAIN_DUMP_AUDIO);
}

void MenuBar::BuildOptionsMenu()
{
  auto* const options = new QtUtils::NonAutodismissibleMenu(tr("&Options"), this);
  addMenu(options);

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
  options->addAction(tr("Co&nfiguration"), QKeySequence::Preferences, this, &MenuBar::Configure);
#else
  options->addAction(tr("Co&nfiguration"), this, &MenuBar::Configure, QKeySequence::Preferences);
#endif
  options->addSeparator();

  options->addAction(tr("&Graphics Settings"), this, &MenuBar::ConfigureGraphics);
  options->addAction(tr("&Audio Settings"), this, &MenuBar::ConfigureAudio);
  options->addAction(tr("&Controller Settings"), this, &MenuBar::ConfigureControllers);
  options->addAction(tr("&Hotkey Settings"), this, &MenuBar::ConfigureHotkeys);
  options->addAction(tr("&Free Look Settings"), this, &MenuBar::ConfigureFreelook);
  options->addSeparator();

  // The rest is only shown in debug mode.
  m_boot_to_pause = options->addAction(tr("Boot to Pause"));
  m_boot_to_pause->setCheckable(true);
  m_boot_to_pause->setChecked(SConfig::GetInstance().bBootToPause);
  connect(m_boot_to_pause, &QAction::toggled, this,
          [](bool pause) { SConfig::GetInstance().bBootToPause = pause; });

  m_reset_panic_ignore = options->addAction(tr("Reset Ignore Panic Handler"));
  connect(m_reset_panic_ignore, &QAction::triggered, this, [] {
    // "Ignore for this session" is a CurrentRun override; dropping it restores panic alerts.
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_USE_PANIC_HANDLERS);
  });

  m_debug_font = options->addAction(tr("&Font..."), this, &MenuBar::ChangeDebugFont);
}

void MenuBar::BuildToolsMenu()
{
  QMenu* const tools = addMenu(tr("&Tools"));

  tools->addAction(tr("&Resource Pack Manager"), this, [this] { emit ShowResourcePackManager(); });
  tools->addAction(tr("&Cheats Manager"), this, [this] { emit ShowCheatsManager(); });
  tools->addAction(tr("&FIFO Player"), this, &MenuBar::ShowFIFOPlayer);

  auto* const usb_devices = new QMenu(tr("&Emulated USB Devices"), tools);
  usb_devices->addAction(tr("&Skylanders Portal"), this, &MenuBar::ShowSkylanderPortal);
  usb_devices->addAction(tr("&Infinity Base"), this, &MenuBar::ShowInfinityBase);
  usb_devices->addAction(tr("&Wii Speak"), this, &MenuBar::ShowWiiSpeakWindow);
  usb_devices->addAction(tr("&Logitech USB Microphone"), this, &MenuBar::ShowLogitechMicWindow);
  tools->addMenu(usb_devices);
  tools->addSeparator();

  tools->addAction(tr("Start &NetPlay..."), this, &MenuBar::StartNetPlay);
  tools->addAction(tr("Browse &NetPlay Sessions...."), this, &MenuBar::BrowseNetPlay);
  tools->addSeparator();

#ifdef USE_RETRO_ACHIEVEMENTS
  tools->addAction(tr("&Achievements"), this, [this] { emit ShowAchievementsWindow(); });
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  // Filled in (and shown) when RAIntegration provides its development menu.
  m_ra_development = tools->addMenu(tr("RetroAchievements Development"));
  m_ra_menu_hook = AchievementManager::GetInstance().dev_menu_update_event.Register(
      [this] { QueueOnObject(this, [this] { UpdateAchievementDevelopmentMenu(); }); });
  m_ra_development->menuAction()->setVisible(false);
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
  tools->addSeparator();
#endif  // USE_RETRO_ACHIEVEMENTS

  QMenu* const gc_main_menu = tools->addMenu(tr("Load &GameCube Main Menu"));
  const auto add_ipl = [this, gc_main_menu](const QString& text, DiscIO::Region region) {
    return gc_main_menu->addAction(text, this, [this, region] { emit BootGameCubeIPL(region); });
  };
  m_gc_ipl[0] = add_ipl(tr("NTSC-&J"), DiscIO::Region::NTSC_J);
  m_gc_ipl[1] = add_ipl(tr("NTSC-&U"), DiscIO::Region::NTSC_U);
  m_gc_ipl[2] = add_ipl(tr("&PAL"), DiscIO::Region::PAL);
  m_gc_ipl[3] = add_ipl(tr("&Triforce"), DiscIO::Region::Unknown);

  tools->addAction(tr("&Memory Card Manager"), this, [this] { emit ShowMemcardManager(); });
  tools->addSeparator();

  // Labelled by UpdateToolsMenu, which knows the installed System Menu version.
  m_boot_system_menu = tools->addAction(QString{}, this, [this] { emit BootWiiSystemMenu(); });
  m_install_wad = tools->addAction(tr("Install &WAD..."), this, &MenuBar::InstallWAD);

  QMenu* const manage_nand = tools->addMenu(tr("Manage &NAND"));
  m_import_nand_backup = manage_nand->addAction(tr("Import &BootMii NAND Backup..."), this,
                                                [this] { emit ImportNANDBackup(); });
  m_check_nand = manage_nand->addAction(tr("Check &NAND..."), this, &MenuBar::CheckNAND);
  manage_nand->addAction(tr("Extract &Certificates from NAND"), this,
                         &MenuBar::NANDExtractCertificates);

  m_boot_system_menu->setEnabled(false);
  connect(&Settings::Instance(), &Settings::NANDRefresh, this,
          [this] { UpdateToolsMenu(Core::State::Uninitialized); });

  m_online_update = tools->addMenu(tr("Perform Online System &Update"));
  const auto add_update = [this](const QString& text, const char* region) {
    return m_online_update->addAction(text, this,
                                      [this, region] { emit PerformOnlineUpdate(region); });
  };
  m_online_update_current_region = add_update(tr("&Current Region"), "");
  m_online_update->addSeparator();
  add_update(tr("&Europe"), "EUR");
  add_update(tr("&Japan"), "JPN");
  add_update(tr("&Korea"), "KOR");
  add_update(tr("&United States"), "USA");
  tools->addSeparator();

  m_import_wii_save = tools->addAction(tr("Import &Wii Save..."), this, &MenuBar::ImportWiiSave);
  m_import_wii_saves =
      tools->addAction(tr("Import &Wii Saves..."), this, &MenuBar::ImportWiiSaves);
  m_export_wii_saves =
      tools->addAction(tr("Export All &Wii Saves"), this, &MenuBar::ExportWiiSaves);

  auto* const remotes = new QtUtils::NonAutodismissibleMenu(tr("&Connect Wii Remotes"), tools);
  tools->addSeparator();
  tools->addMenu(remotes);

  for (int index = 0; index < 5; ++index)
  {
    if (index == 4)
      remotes->addSeparator();
    const QString text =
        index < 4 ? tr("Connect Wii Remote &%1").arg(index + 1) : tr("Connect &Balance Board");
    QAction* const entry =
        remotes->addAction(text, this, [this, index] { emit ConnectWiiRemote(index); });
    entry->setCheckable(true);
    m_wii_remotes[static_cast<std::size_t>(index)] = entry;
  }
}

void MenuBar::BuildViewMenu()
{
  auto* const view = new QtUtils::NonAutodismissibleMenu(tr("&View"), this);
  addMenu(view);

  AddSettingsToggle(view, tr("Show &Log"), &Settings::IsLogVisible, &Settings::SetLogVisible,
                    &Settings::LogVisibilityChanged);
  AddSettingsToggle(view, tr("Show Log &Configuration"), &Settings::IsLogConfigVisible,
                    &Settings::SetLogConfigVisible, &Settings::LogConfigVisibilityChanged);
  AddSettingsToggle(view, tr("Show &Toolbar"), &Settings::IsToolBarVisible,
                    &Settings::SetToolBarVisible, &Settings::ToolBarVisibilityChanged);
  AddSettingsToggle(view, tr("&Lock Widgets In Place"), &Settings::AreWidgetsLocked,
                    &Settings::SetWidgetsLocked);
  view->addSeparator();

  BuildDebuggerPanelToggles(view);
  view->addSeparator();

  BuildGameListViewChoice(view);
  view->addSeparator();
  BuildGameListFilterMenus(view);

  view->addSeparator();
  AddSettingsToggle(view, tr("Show Game Count"), &Settings::IsGameCountVisible,
                    &Settings::SetGameCountVisible);

  view->addSeparator();
  // Purging is offered once the game list has started scanning, and not again until the next
  // requested refresh has started.
  QAction* const purge =
      view->addAction(tr("Purge Game List Cache"), this, &MenuBar::PurgeGameListCache);
  purge->setEnabled(false);
  connect(&Settings::Instance(), &Settings::GameListRefreshRequested, purge,
          [purge] { purge->setEnabled(false); });
  connect(&Settings::Instance(), &Settings::GameListRefreshStarted, purge,
          [purge] { purge->setEnabled(true); });
  view->addSeparator();

#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
  view->addAction(tr("Search"), QKeySequence::Find, this, &MenuBar::ShowSearch);
#else
  view->addAction(tr("Search"), this, &MenuBar::ShowSearch, QKeySequence::Find);
#endif
}

void MenuBar::BuildDebuggerPanelToggles(QMenu* view)
{
  // Shown in debug mode only (see ReflectDebugMode).
  m_debugger_panels = {
      AddSettingsToggle(view, tr("&Code"), &Settings::IsCodeVisible, &Settings::SetCodeVisible,
                        &Settings::CodeVisibilityChanged),
      AddSettingsToggle(view, tr("&Registers"), &Settings::IsRegistersVisible,
                        &Settings::SetRegistersVisible, &Settings::RegistersVisibilityChanged),
      AddSettingsToggle(view, tr("&Threads"), &Settings::IsThreadsVisible,
                        &Settings::SetThreadsVisible, &Settings::ThreadsVisibilityChanged),
      // i18n: A "watch" here is a list of memory locations whose values are being observed, not
      // a clock.
      AddSettingsToggle(view, tr("&Watch"), &Settings::IsWatchVisible, &Settings::SetWatchVisible,
                        &Settings::WatchVisibilityChanged),
      AddSettingsToggle(view, tr("&Breakpoints"), &Settings::IsBreakpointsVisible,
                        &Settings::SetBreakpointsVisible, &Settings::BreakpointsVisibilityChanged),
      AddSettingsToggle(view, tr("&Memory"), &Settings::IsMemoryVisible,
                        &Settings::SetMemoryVisible, &Settings::MemoryVisibilityChanged),
      AddSettingsToggle(view, tr("&Network"), &Settings::IsNetworkVisible,
                        &Settings::SetNetworkVisible, &Settings::NetworkVisibilityChanged),
      AddSettingsToggle(view, tr("&JIT"), &Settings::IsJITVisible, &Settings::SetJITVisible,
                        &Settings::JITVisibilityChanged),
      AddSettingsToggle(view, tr("&Assembler"), &Settings::IsAssemblerVisible,
                        &Settings::SetAssemblerVisible, &Settings::AssemblerVisibilityChanged),
  };
}

void MenuBar::BuildGameListViewChoice(QMenu* view)
{
  QAction* const as_list = view->addAction(tr("List View"));
  as_list->setCheckable(true);
  QAction* const as_grid = view->addAction(tr("Grid View"));
  as_grid->setCheckable(true);

  auto* const choice = new QActionGroup(this);
  choice->addAction(as_list);
  choice->addAction(as_grid);

  const bool list_preferred = Settings::Instance().GetPreferredView();
  as_list->setChecked(list_preferred);
  as_grid->setChecked(!list_preferred);

  connect(as_list, &QAction::triggered, this, &MenuBar::ShowList);
  connect(as_grid, &QAction::triggered, this, &MenuBar::ShowGrid);
}

void MenuBar::BuildGameListFilterMenus(QMenu* view)
{
  // Entries are listed alphabetically by their (translated) label.
  using Entries = QMap<QString, const Config::Info<bool>*>;

  // List Columns: each entry shows or hides a column of the list view.
  const Entries columns{
      {tr("Platform"), &Config::MAIN_GAMELIST_COLUMN_PLATFORM},
      {tr("Banner"), &Config::MAIN_GAMELIST_COLUMN_BANNER},
      {tr("Title"), &Config::MAIN_GAMELIST_COLUMN_TITLE},
      {tr("Description"), &Config::MAIN_GAMELIST_COLUMN_DESCRIPTION},
      {tr("Maker"), &Config::MAIN_GAMELIST_COLUMN_MAKER},
      {tr("File Name"), &Config::MAIN_GAMELIST_COLUMN_FILE_NAME},
      {tr("File Path"), &Config::MAIN_GAMELIST_COLUMN_FILE_PATH},
      {tr("Game ID"), &Config::MAIN_GAMELIST_COLUMN_GAME_ID},
      {tr("Region"), &Config::MAIN_GAMELIST_COLUMN_REGION},
      {tr("File Size"), &Config::MAIN_GAMELIST_COLUMN_FILE_SIZE},
      {tr("File Format"), &Config::MAIN_GAMELIST_COLUMN_FILE_FORMAT},
      {tr("Block Size"), &Config::MAIN_GAMELIST_COLUMN_BLOCK_SIZE},
      {tr("Compression"), &Config::MAIN_GAMELIST_COLUMN_COMPRESSION},
      {tr("Time Played"), &Config::MAIN_GAMELIST_COLUMN_TIME_PLAYED},
      {tr("Tags"), &Config::MAIN_GAMELIST_COLUMN_TAGS},
  };
  // Show Platforms / Show Regions: each entry filters the game list.
  const Entries platforms{
      {tr("Show Wii"), &Config::MAIN_GAMELIST_LIST_WII},
      {tr("Show GameCube"), &Config::MAIN_GAMELIST_LIST_GC},
      {tr("Show Triforce"), &Config::MAIN_GAMELIST_LIST_TRI},
      {tr("Show WAD"), &Config::MAIN_GAMELIST_LIST_WAD},
      {tr("Show ELF/DOL"), &Config::MAIN_GAMELIST_LIST_ELF_DOL},
  };
  const Entries regions{
      {tr("Show JPN"), &Config::MAIN_GAMELIST_LIST_JPN},
      {tr("Show PAL"), &Config::MAIN_GAMELIST_LIST_PAL},
      {tr("Show USA"), &Config::MAIN_GAMELIST_LIST_USA},
      {tr("Show Australia"), &Config::MAIN_GAMELIST_LIST_AUSTRALIA},
      {tr("Show France"), &Config::MAIN_GAMELIST_LIST_FRANCE},
      {tr("Show Germany"), &Config::MAIN_GAMELIST_LIST_GERMANY},
      {tr("Show Italy"), &Config::MAIN_GAMELIST_LIST_ITALY},
      {tr("Show Korea"), &Config::MAIN_GAMELIST_LIST_KOREA},
      {tr("Show Netherlands"), &Config::MAIN_GAMELIST_LIST_NETHERLANDS},
      {tr("Show Russia"), &Config::MAIN_GAMELIST_LIST_RUSSIA},
      {tr("Show Spain"), &Config::MAIN_GAMELIST_LIST_SPAIN},
      {tr("Show Taiwan"), &Config::MAIN_GAMELIST_LIST_TAIWAN},
      {tr("Show World"), &Config::MAIN_GAMELIST_LIST_WORLD},
      {tr("Show Unknown"), &Config::MAIN_GAMELIST_LIST_UNKNOWN},
  };

  using Notifier = void (MenuBar::*)(const QString&, bool);
  const auto fill = [this](QMenu* menu, const Entries& entries, Notifier notify) {
    auto* const group = new QActionGroup(this);
    group->setExclusive(false);
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
    {
      const QString label = it.key();
      const Config::Info<bool>* const setting = it.value();
      QAction* const entry = group->addAction(menu->addAction(label));
      entry->setCheckable(true);
      entry->setChecked(Config::Get(*setting));
      connect(entry, &QAction::toggled, this, [this, setting, label, notify](bool shown) {
        Config::SetBase(*setting, shown);
        emit(this->*notify)(label, shown);
      });
    }
  };

  m_list_columns = new QtUtils::NonAutodismissibleMenu(tr("List Columns"), view);
  view->addMenu(m_list_columns);
  fill(m_list_columns, columns, &MenuBar::ColumnVisibilityToggled);
  view->addSeparator();

  auto* const platform_menu = new QtUtils::NonAutodismissibleMenu(tr("Show Platforms"), view);
  view->addMenu(platform_menu);
  fill(platform_menu, platforms, &MenuBar::GameListPlatformVisibilityToggled);

  // Regions also have "Show All" and "Hide All". Each region entry applies a new state the same
  // way whether it came from the entry itself or from those two.
  auto* const region_menu = new QtUtils::NonAutodismissibleMenu(tr("Show Regions"), view);
  view->addMenu(region_menu);
  const QAction* const show_all = region_menu->addAction(tr("Show All"));
  const QAction* const hide_all = region_menu->addAction(tr("Hide All"));
  region_menu->addSeparator();

  for (auto it = regions.cbegin(); it != regions.cend(); ++it)
  {
    const QString label = it.key();
    const Config::Info<bool>* const setting = it.value();
    QAction* const entry = region_menu->addAction(label);
    entry->setCheckable(true);
    entry->setChecked(Config::Get(*setting));

    const auto apply = [this, entry, setting, label](bool shown) {
      entry->setChecked(shown);
      Config::SetBase(*setting, shown);
      emit GameListRegionVisibilityToggled(label, shown);
    };
    connect(entry, &QAction::toggled, this, apply);
    connect(show_all, &QAction::triggered, entry, [apply] { apply(true); });
    connect(hide_all, &QAction::triggered, entry, [apply] { apply(false); });
  }
}

void MenuBar::BuildJITMenu()
{
  m_jit = new QtUtils::NonAutodismissibleMenu(tr("JIT"), this);
  addMenu(m_jit);

  m_jit_interpreter = m_jit->addAction(tr("Interpreter Core"));
  m_jit_interpreter->setCheckable(true);
  m_jit_interpreter->setChecked(Config::Get(Config::MAIN_CPU_CORE) ==
                                PowerPC::CPUCore::Interpreter);
  connect(m_jit_interpreter, &QAction::toggled, this, [](bool interpreter) {
    TheSystem().GetPowerPC().SetMode(interpreter ? PowerPC::CoreMode::Interpreter :
                                                   PowerPC::CoreMode::JIT);
  });
  m_jit->addSeparator();

  // These two live in SConfig and only take effect for newly compiled blocks.
  const auto add_cache_switch = [this](const QString& text, bool SConfig::*flag) {
    QAction* const action = m_jit->addAction(text);
    action->setCheckable(true);
    action->setChecked(SConfig::GetInstance().*flag);
    connect(action, &QAction::toggled, this, [this, flag](bool on) {
      SConfig::GetInstance().*flag = on;
      ClearJitCache();
    });
    return action;
  };
  m_jit_no_block_linking = add_cache_switch(tr("JIT Block Linking Off"), &SConfig::bJITNoBlockLinking);
  m_jit_no_block_cache = add_cache_switch(tr("Disable JIT Cache"), &SConfig::bJITNoBlockCache);

  AddConfigToggle(m_jit, tr("Disable Fastmem"), Config::MAIN_FASTMEM, true);
  AddConfigToggle(m_jit, tr("Disable Page Table Fastmem"), Config::MAIN_PAGE_TABLE_FASTMEM, true);
  m_jit_no_fastmem_arena =
      AddConfigToggle(m_jit, tr("Disable Fastmem Arena"), Config::MAIN_FASTMEM_ARENA, true);
  m_jit_no_large_entry_map = AddConfigToggle(m_jit, tr("Disable Large Entry Points Map"),
                                             Config::MAIN_LARGE_ENTRY_POINTS_MAP, true);
  m_jit_clear_cache = m_jit->addAction(tr("Clear Cache"), this, &MenuBar::ClearJitCache);
  m_jit->addSeparator();

  m_jit_log_coverage = m_jit->addAction(tr("Log JIT Instruction Coverage"), this,
                                        [] { PPCTables::LogCompiledInstructions(); });
  m_jit_search =
      m_jit->addAction(tr("Search for an Instruction"), this, &MenuBar::SearchForInstruction);
  m_jit->addSeparator();

  m_jit_profiling = AddConfigToggle(m_jit, tr("Enable JIT Block Profiling"),
                                    Config::MAIN_DEBUG_JIT_ENABLE_PROFILING);
  m_jit_wipe_profiling = m_jit->addAction(tr("Wipe JIT Block Profiling Data"), this,
                                          &MenuBar::WipeJitProfilingData);
  m_jit_block_dump =
      m_jit->addAction(tr("Write JIT Block Log Dump"), this, &MenuBar::WriteJitBlockLogDump);
  m_jit->addSeparator();

  // Per-category switches that make the JIT fall back to the interpreter.
  AddConfigToggle(m_jit, tr("JIT Off (JIT Core)"), Config::MAIN_DEBUG_JIT_OFF);
  AddConfigToggle(m_jit, tr("JIT LoadStore Off"), Config::MAIN_DEBUG_JIT_LOAD_STORE_OFF);
  AddConfigToggle(m_jit, tr("JIT LoadStore lbzx Off"), Config::MAIN_DEBUG_JIT_LOAD_STORE_LBZX_OFF);
  AddConfigToggle(m_jit, tr("JIT LoadStore lXz Off"), Config::MAIN_DEBUG_JIT_LOAD_STORE_LXZ_OFF);
  AddConfigToggle(m_jit, tr("JIT LoadStore lwz Off"), Config::MAIN_DEBUG_JIT_LOAD_STORE_LWZ_OFF);
  AddConfigToggle(m_jit, tr("JIT LoadStore Floating Off"),
                  Config::MAIN_DEBUG_JIT_LOAD_STORE_FLOATING_OFF);
  AddConfigToggle(m_jit, tr("JIT LoadStore Paired Off"),
                  Config::MAIN_DEBUG_JIT_LOAD_STORE_PAIRED_OFF);
  AddConfigToggle(m_jit, tr("JIT FloatingPoint Off"), Config::MAIN_DEBUG_JIT_FLOATING_POINT_OFF);
  AddConfigToggle(m_jit, tr("JIT Integer Off"), Config::MAIN_DEBUG_JIT_INTEGER_OFF);
  AddConfigToggle(m_jit, tr("JIT Paired Off"), Config::MAIN_DEBUG_JIT_PAIRED_OFF);
  AddConfigToggle(m_jit, tr("JIT SystemRegisters Off"),
                  Config::MAIN_DEBUG_JIT_SYSTEM_REGISTERS_OFF);
  AddConfigToggle(m_jit, tr("JIT Branch Off"), Config::MAIN_DEBUG_JIT_BRANCH_OFF);
  AddConfigToggle(m_jit, tr("JIT Register Cache Off"), Config::MAIN_DEBUG_JIT_REGISTER_CACHE_OFF);
}

void MenuBar::BuildSymbolsMenu()
{
  m_symbols = addMenu(tr("Symbols"));

  m_symbols->addAction(tr("&Clear Symbols"), this, &MenuBar::ClearSymbols);
  QMenu* const generate = m_symbols->addMenu(tr("&Generate Symbols From"));
  generate->addAction(tr("Address"), this, &MenuBar::GenerateSymbolsFromAddress);
  generate->addAction(tr("Signature Database"), this, &MenuBar::GenerateSymbolsFromSignatureDB);
  generate->addAction(tr("RSO Modules"), this, &MenuBar::GenerateSymbolsFromRSO);
  QAction* const demangled = m_symbols->addAction(tr("Show &Demangled Names"));
  m_symbols->addSeparator();

  m_symbols->addAction(tr("&Load Symbol Map"), this, &MenuBar::LoadSymbolMap);
  m_symbols->addAction(tr("&Save Symbol Map"), this, &MenuBar::SaveSymbolMap);
  m_symbols->addSeparator();

  m_symbols->addAction(tr("Load &Other Map File..."), this,
                       [this] { LoadMapFileChosenByUser(false); });
  m_symbols->addAction(tr("Load &Bad Map File..."), this,
                       [this] { LoadMapFileChosenByUser(true); });
  m_symbols->addAction(tr("Save Symbol Map &As..."), this, &MenuBar::SaveSymbolMapAs);
  m_symbols->addSeparator();

  m_symbols->addAction(tr("Sa&ve Code"), this, &MenuBar::SaveCode);
  m_symbols->addSeparator();

  m_symbols->addAction(tr("C&reate Signature File..."), this, &MenuBar::CreateSignatureFile);
  m_symbols->addAction(tr("Append to &Existing Signature File..."), this,
                       &MenuBar::AppendSignatureFile);
  m_symbols->addAction(tr("Combine &Two Signature Files..."), this,
                       &MenuBar::CombineSignatureFiles);
  m_symbols->addAction(tr("Appl&y Signature File..."), this, &MenuBar::ApplySignatureFile);
  m_symbols->addSeparator();

  m_symbols->addAction(tr("&Patch HLE Functions"), this,
                       [] { HLE::PatchFunctions(TheSystem()); });

  demangled->setCheckable(true);
  demangled->setChecked(Settings::Instance().IsShowDemangledNames());
  connect(demangled, &QAction::toggled, &Settings::Instance(), &Settings::SetShowDemangledNames);
  connect(&Settings::Instance(), &Settings::ShowDemangledNamesChanged, demangled,
          &QAction::setChecked);
}

void MenuBar::BuildHelpMenu()
{
  QMenu* const help = addMenu(tr("&Help"));

  connect(help->addAction(tr("&Website")), &QAction::triggered, this,
          [] { OpenWebPage("https://dolphin-emu.org/"); });
  connect(help->addAction(tr("Online &Documentation")), &QAction::triggered, this,
          [] { OpenWebPage("https://dolphin-emu.org/docs/guides"); });
  connect(help->addAction(tr("&GitHub Repository")), &QAction::triggered, this,
          [] { OpenWebPage("https://github.com/dolphin-emu/dolphin"); });
  connect(help->addAction(tr("&Bug Tracker")), &QAction::triggered, this,
          [] { OpenWebPage("https://bugs.dolphin-emu.org/projects/emulator"); });

  if (AutoUpdateChecker::SystemSupportsAutoUpdates())
  {
    help->addSeparator();
    help->addAction(tr("&Check for Updates..."), this, &MenuBar::InstallUpdateManually);
  }

#ifndef __APPLE__
  help->addSeparator();
#endif
  help->addAction(tr("&About"), this, &MenuBar::ShowAboutDialog);
}

QString MenuBar::SignatureFileFilter() const
{
  return QStringLiteral("%1 (*.dsy);; %2 (*.csv);; %3 (*.mega)")
      .arg(tr("Dolphin Signature File"), tr("Dolphin Signature CSV File"),
           tr("WiiTools Signature MEGA File"));
}

// --- Keeping the menus current ---------------------------------------------------------------

void MenuBar::ReflectEmulationState(Core::State state)
{
  const bool has_game = state != Core::State::Uninitialized;
  const bool unpaused = has_game && state != Core::State::Paused;

  m_eject_disc->setEnabled(has_game);
  m_change_disc->setEnabled(has_game);

  // Play and Pause take turns; Stop only appears with a game.
  m_play->setEnabled(!unpaused);
  m_play->setVisible(!unpaused);
  m_pause->setEnabled(unpaused);
  m_pause->setVisible(unpaused);
  m_stop->setEnabled(has_game);
  m_stop->setVisible(has_game);
  m_reset->setEnabled(has_game);
  m_fullscreen->setEnabled(has_game);
  m_screenshot->setEnabled(has_game);
  m_save_state->setEnabled(has_game);

  // Hardcore achievements forbid loading states and frame advance.
  const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
  m_load_state->setEnabled(has_game && !hardcore);
  m_frame_advance->setEnabled(has_game && !hardcore);

  m_read_only->setEnabled(has_game);
  if (!has_game)
  {
    m_stop_recording->setEnabled(false);
    m_export_recording->setEnabled(false);
  }
  // A recording starts either at boot (a game is selected and nothing runs) or from a state.
  const bool can_record_from_boot = m_game_selected && state == Core::State::Uninitialized;
  const bool can_record_from_state =
      state == Core::State::Running || state == Core::State::Paused;
  m_play_recording->setEnabled(can_record_from_boot && !hardcore);
  m_start_recording->setEnabled((can_record_from_boot || can_record_from_state) &&
                                !TheSystem().GetMovie().IsPlayingInput());

  // Settings that need a stopped core vs. actions that need a running one.
  const bool jit_present = TheSystem().GetJitInterface().GetCore() != nullptr;
  m_jit_interpreter->setEnabled(has_game);
  m_jit_no_block_linking->setEnabled(!has_game);
  m_jit_no_block_cache->setEnabled(!has_game);
  m_jit_no_fastmem_arena->setEnabled(!has_game);
  m_jit_no_large_entry_map->setEnabled(!has_game);
  m_jit_clear_cache->setEnabled(has_game);
  m_jit_log_coverage->setEnabled(!has_game);
  m_jit_search->setEnabled(has_game);
  m_jit_wipe_profiling->setEnabled(jit_present);
  m_jit_block_dump->setEnabled(jit_present);

  m_symbols->setEnabled(has_game);

  RelabelStateSlots();
  UpdateToolsMenu(state);

  ReflectDebugMode(Settings::Instance().IsDebugModeEnabled());
}

void MenuBar::ReflectConfig()
{
  SignalBlocking(m_jit_profiling)->setChecked(Config::Get(Config::MAIN_DEBUG_JIT_ENABLE_PROFILING));
  SignalBlocking(m_movie_window)->setChecked(Config::Get(Config::MAIN_MOVIE_SHOW_OSD));
}

void MenuBar::ReflectDebugMode(bool enabled)
{
  for (QAction* const action : {m_boot_to_pause, m_reset_panic_ignore, m_debug_font})
    action->setVisible(enabled);
  for (QAction* const action : m_debugger_panels)
    action->setVisible(enabled);

  // The JIT and Symbols menus are (re)appended at the end of the bar in debug mode.
  if (enabled)
  {
    addMenu(m_jit);
    addMenu(m_symbols);
  }
  else
  {
    removeAction(m_jit->menuAction());
    removeAction(m_symbols->menuAction());
  }
}

void MenuBar::ReflectSelectedGame(const std::shared_ptr<const UICommon::GameFile>& game_file)
{
  m_game_selected = game_file != nullptr;

  auto& system = TheSystem();
  const bool from_boot = m_game_selected && Core::IsUninitialized(system);
  const bool from_state = Core::IsRunning(system);
  m_play_recording->setEnabled(from_boot);
  m_start_recording->setEnabled((from_boot || from_state) && !system.GetMovie().IsPlayingInput());
}

void MenuBar::ReflectRecordingStatus(bool recording)
{
  auto& system = TheSystem();
  const bool from_boot = m_game_selected && Core::IsUninitialized(system);
  const bool from_state = Core::IsRunning(system);

  m_start_recording->setEnabled(!recording && (from_boot || from_state));
  m_stop_recording->setEnabled(recording);
  m_export_recording->setEnabled(recording);
}

void MenuBar::RelabelStateSlots()
{
  const QList<QAction*> select = m_slot_choices->actions();
  const QList<QAction*> load = m_load_from_slot->actions();
  const QList<QAction*> save = m_save_to_slot->actions();
  for (qsizetype index = 0; index < select.size(); ++index)
  {
    const int slot = static_cast<int>(index) + 1;
    const QString contents =
        QString::fromStdString(State::GetInfoStringOfSlot(static_cast<u32>(slot)));
    load[index]->setText(tr("Load from Slot %1 - %2").arg(slot).arg(contents));
    save[index]->setText(tr("Save to Slot %1 - %2").arg(slot).arg(contents));
    select[index]->setText(tr("Select Slot %1 - %2").arg(slot).arg(contents));
  }
}

void MenuBar::UpdateToolsMenu(const Core::State state)
{
  const bool stopped = state == Core::State::Uninitialized;
  const bool running = state == Core::State::Running || state == Core::State::Paused;

  // NAND, save and boot tools need the core stopped.
  m_boot_system_menu->setEnabled(stopped);
  m_online_update->setEnabled(stopped);
  static constexpr std::array<const char*, 4> IPL_REGION_DIRS{JAP_DIR, USA_DIR, EUR_DIR, DEV_DIR};
  for (std::size_t i = 0; i < m_gc_ipl.size(); ++i)
    m_gc_ipl[i]->setEnabled(stopped && File::Exists(Config::GetBootROMPath(IPL_REGION_DIRS[i])));
  for (QAction* const action : {m_install_wad, m_import_nand_backup, m_check_nand,
                                m_import_wii_save, m_import_wii_saves, m_export_wii_saves})
  {
    action->setEnabled(stopped);
  }

  if (stopped)
  {
    // Look at the NAND for an installed System Menu.
    IOS::HLE::Kernel ios;
    const auto tmd = ios.GetESCore().FindInstalledTMD(Titles::SYSTEM_MENU);
    const bool installed = tmd.IsValid();

    QString version;
    if (installed)
    {
      version = QString::fromStdString(
          DiscIO::GetSysMenuVersionString(tmd.GetTitleVersion(), tmd.IsvWii()));
    }
    const QString label = (installed && tmd.IsvWii()) ? tr("Load vWii &System Menu %1") :
                                                        tr("Load Wii &System Menu %1");
    m_boot_system_menu->setText(label.arg(version));
    m_boot_system_menu->setEnabled(installed);

    // Without a System Menu only a region-specific update can install one.
    for (QAction* const action : m_online_update->actions())
      action->setEnabled(!installed);
    m_online_update_current_region->setEnabled(installed);
  }

  // Remotes can be connected while an emulated Bluetooth adapter is in use.
  const auto bluetooth = WiiUtils::GetBluetoothEmuDevice();
  const bool remotes_available = running && bluetooth != nullptr;
  for (std::size_t index = 0; index < m_wii_remotes.size(); ++index)
  {
    m_wii_remotes[index]->setEnabled(remotes_available);
    if (remotes_available)
      m_wii_remotes[index]->setChecked(bluetooth->AccessWiimoteByIndex(index)->IsConnected());
  }
}

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
void MenuBar::UpdateAchievementDevelopmentMenu()
{
  const rc_client_raintegration_menu_t* const ra_menu =
      AchievementManager::GetInstance().GetDevelopmentMenu();
  if (ra_menu == nullptr)
  {
    m_ra_development->menuAction()->setVisible(false);
    return;
  }

  m_ra_development->menuAction()->setVisible(true);
  m_ra_development->clear();
  for (u32 index = 0; index < ra_menu->num_items; ++index)
  {
    const auto& item = ra_menu->items[index];
    // Items without a label are separators.
    if (item.label == nullptr)
    {
      m_ra_development->addSeparator();
      continue;
    }

    QAction* const action = m_ra_development->addAction(
        QString::fromStdString(item.label), this,
        [id = item.id] { AchievementManager::GetInstance().ActivateDevMenuItem(id); });
    action->setEnabled(item.enabled);
    // RAIntegration's first two items are toggles; the rest are plain commands.
    action->setCheckable(index < 2);
    action->setChecked(item.checked);
  }
}
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

void MenuBar::InstallUpdateManually()
{
  // Without a configured track, a manual check looks at development builds.
  std::string track = Config::Get(Config::MAIN_AUTOUPDATE_UPDATE_TRACK);
  if (track.empty())
    track = "dev";

  auto* const updater =
      new Updater(parentWidget(), track, Config::Get(Config::MAIN_AUTOUPDATE_HASH_OVERRIDE));
  updater->CheckForUpdate();
}

// --- Tools -------------------------------------------------------------------------------------

void MenuBar::InstallWAD()
{
  const QString wad = DolphinFileDialog::getOpenFileName(
      this, tr("Select Title to Install to NAND"), QString(), tr("WAD files (*.wad)"));
  if (wad.isEmpty())
    return;

  if (!WiiUtils::InstallWAD(wad.toStdString()))
  {
    ModalMessageBox::critical(this, tr("Failure"), tr("Failed to install this title to the NAND."));
    return;
  }

  Settings::Instance().NANDRefresh();
  ModalMessageBox::information(this, tr("Success"),
                               tr("Successfully installed this title to the NAND."));
}

void MenuBar::ImportWiiSave()
{
  const QString source =
      DolphinFileDialog::getOpenFileName(this, tr("Select Save File"), QDir::currentPath(),
                                         tr("Wii save files (*.bin);;"
                                            "All Files (*)"));
  if (source.isEmpty())
    return;

  const auto may_overwrite = [this] {
    const auto answer = ModalMessageBox::question(
        this, tr("Save Import"),
        tr("Save data for this title already exists in the NAND. Consider backing up "
           "the current data before overwriting.\n\nOverwrite existing save data?"));
    return answer == QMessageBox::Yes;
  };

  const QString title = tr("Save Import");
  switch (WiiSave::Import(source.toStdString(), may_overwrite))
  {
  case WiiSave::CopyResult::Success:
    ModalMessageBox::information(this, title, tr("Successfully imported save file."));
    break;
  case WiiSave::CopyResult::Cancelled:
    break;
  case WiiSave::CopyResult::CorruptedSource:
    ModalMessageBox::critical(this, title,
                              tr("Failed to import save file. The given file appears to be "
                                 "corrupted or is not a valid Wii save."));
    break;
  case WiiSave::CopyResult::TitleMissing:
    ModalMessageBox::critical(
        this, title,
        tr("Failed to import save file. Please launch the game once, then try again."));
    break;
  default:
    ModalMessageBox::critical(
        this, title,
        tr("Failed to import save file. Your NAND may be corrupt, or something is preventing "
           "access to files within it. Try repairing your NAND (Tools -> Manage NAND -> Check "
           "NAND...), then import the save again."));
    break;
  }
}

void MenuBar::ImportWiiSaves()
{
  const QString folder =
      DolphinFileDialog::getExistingDirectory(this, tr("Select Save Folder"), QDir::currentPath());
  if (folder.isEmpty())
    return;

  // "Yes to all" / "No to all" answers stick for the rest of the folder.
  enum class Standing
  {
    Ask,
    AlwaysOverwrite,
    NeverOverwrite,
  };
  Standing standing = Standing::Ask;

  std::size_t imported = 0;
  std::size_t failed = 0;
  QStringList failures;

  QDirIterator files(folder, QStringList(QStringLiteral("*.bin")), QDir::Files,
                     QDirIterator::Subdirectories);
  while (files.hasNext())
  {
    const QString file = files.next();

    const auto may_overwrite = [this, &standing, &file] {
      if (standing != Standing::Ask)
        return standing == Standing::AlwaysOverwrite;

      const auto answer = ModalMessageBox::question(
          this, tr("Save Import"),
          tr("%1: Save data for this title already exists in the NAND. Consider backing up "
             "the current data before overwriting.\n\nOverwrite existing save data?")
              .arg(file),
          QMessageBox::StandardButton::YesAll | QMessageBox::StandardButton::Yes |
              QMessageBox::StandardButton::No | QMessageBox::StandardButton::NoAll);
      if (answer == QMessageBox::YesAll)
        standing = Standing::AlwaysOverwrite;
      else if (answer == QMessageBox::NoAll)
        standing = Standing::NeverOverwrite;
      return answer == QMessageBox::YesAll || answer == QMessageBox::Yes;
    };

    QString problem;
    switch (WiiSave::Import(file.toStdString(), may_overwrite))
    {
    case WiiSave::CopyResult::Success:
      ++imported;
      continue;
    case WiiSave::CopyResult::Cancelled:
      continue;
    case WiiSave::CopyResult::CorruptedSource:
      problem = tr("%1: Failed to import save file. The given file appears to be "
                   "corrupted or is not a valid Wii save.");
      break;
    case WiiSave::CopyResult::TitleMissing:
      problem = tr("%1: Failed to import save file. Please launch the game once, then try again.");
      break;
    default:
      problem =
          tr("%1: Failed to import save file. Your NAND may be corrupt, or something is preventing "
             "access to files within it. Try repairing your NAND (Tools -> Manage NAND -> Check "
             "NAND...), then import the save again.");
      break;
    }
    ++failed;
    failures.append(problem.arg(file));
  }

  if (imported == 0 && failed == 0)
    return;

  ModalMessageBox::information(
      this, tr("Save Import"),
      tr("Successfully imported %1 save file(s) with %2 failure(s)").arg(imported).arg(failed),
      QMessageBox::Ok, QMessageBox::NoButton, Qt::WindowModal,
      failures.join(QStringLiteral("\n\n")));
}

void MenuBar::ExportWiiSaves()
{
  const QString destination = DolphinFileDialog::getExistingDirectory(
      this, tr("Select Export Directory"), QString::fromStdString(File::GetUserPath(D_USER_IDX)),
      QFileDialog::ShowDirsOnly);
  if (destination.isEmpty())
    return;

  const std::size_t exported = WiiSave::ExportAll(destination.toStdString());
  ModalMessageBox::information(this, tr("Save Export"),
                               tr("Exported %n save(s)", "", static_cast<int>(exported)));
}

void MenuBar::CheckNAND()
{
  namespace FS = IOS::HLE::FS;

  IOS::HLE::Kernel ios;
  const WiiUtils::NANDCheckResult result = WiiUtils::CheckNAND(ios);

  if (result.bad)
  {
    {
      NANDRepairDialog dialog(result, this);
      if (dialog.exec() != QDialog::Accepted)
        return;
    }

    if (WiiUtils::RepairNAND(ios))
    {
      ModalMessageBox::information(this, tr("NAND Check"), tr("The NAND has been repaired."));
    }
    else
    {
      ModalMessageBox::critical(this, tr("NAND Check"),
                                tr("The NAND could not be repaired. It is recommended to back up "
                                   "your current data and start over with a fresh NAND."));
    }
    return;
  }

  // Healthy: report how full each part of the NAND is, in blocks and KiB.
  const QString user_usage =
      tr("The user-accessible part of your NAND contains %1 blocks (%2 KiB) "
         "of data, out of an allowed maximum of %3 blocks (%4 KiB).")
          .arg(Common::AlignUp(result.used_clusters_user, FS::CLUSTERS_PER_BLOCK) /
               FS::CLUSTERS_PER_BLOCK)
          .arg((result.used_clusters_user * FS::CLUSTER_SIZE) / 1024)
          .arg(FS::USER_CLUSTERS / FS::CLUSTERS_PER_BLOCK)
          .arg((FS::USER_CLUSTERS * FS::CLUSTER_SIZE) / 1024);
  const QString system_usage =
      tr("The system-reserved part of your NAND contains %1 blocks (%2 KiB) "
         "of data, out of an allowed maximum of %3 blocks (%4 KiB).")
          .arg(Common::AlignUp(result.used_clusters_system, FS::CLUSTERS_PER_BLOCK) /
               FS::CLUSTERS_PER_BLOCK)
          .arg((result.used_clusters_system * FS::CLUSTER_SIZE) / 1024)
          .arg(FS::SYSTEM_CLUSTERS / FS::CLUSTERS_PER_BLOCK)
          .arg((FS::SYSTEM_CLUSTERS * FS::CLUSTER_SIZE) / 1024);

  const bool too_full = result.used_clusters_user > FS::USER_CLUSTERS ||
                        result.used_clusters_system > FS::SYSTEM_CLUSTERS;
  const QString headline =
      too_full ? tr("Your NAND contains more data than allowed. Wii "
                    "software may behave incorrectly or not allow saving.") :
                 tr("No issues have been detected.");
  const QString report = QStringLiteral("<b>%1</b><br/><br/>%2<br/><br/>%3")
                             .arg(headline)
                             .arg(user_usage)
                             .arg(system_usage);
  if (too_full)
    ModalMessageBox::warning(this, tr("NAND Check"), report);
  else
    ModalMessageBox::information(this, tr("NAND Check"), report);
}

void MenuBar::NANDExtractCertificates()
{
  if (!DiscIO::NANDImporter().ExtractCertificates())
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to extract certificates from NAND"));
    return;
  }
  ModalMessageBox::information(this, tr("Success"),
                               tr("Successfully extracted certificates from NAND"));
}

void MenuBar::ChangeDebugFont()
{
  bool chosen = false;
  const QFont font = QFontDialog::getFont(&chosen, Settings::Instance().GetDebugFont(), this,
                                          tr("Pick a debug font"));
  if (chosen)
    Settings::Instance().SetDebugFont(font);
}

// --- JIT ---------------------------------------------------------------------------------------

void MenuBar::ClearJitCache()
{
  auto& system = TheSystem();
  system.GetJitInterface().ClearCache(Core::CPUThreadGuard{system});
}

void MenuBar::WipeJitProfilingData()
{
  auto& system = TheSystem();
  system.GetJitInterface().WipeBlockProfilingData(Core::CPUThreadGuard{system});
}

void MenuBar::WriteJitBlockLogDump()
{
  const std::string path = fmt::format(
      "{}{}.txt", File::GetUserPath(D_DUMPDEBUG_JITBLOCKS_IDX), SConfig::GetInstance().GetGameID());
  const QString shown_path = QString::fromStdString(path);

  File::IOFile dump(path, "w");
  if (!dump)
  {
    ModalMessageBox::warning(this, tr("Error"),
                             tr("Failed to open \"%1\" for writing.").arg(shown_path));
    return;
  }

  auto& system = TheSystem();
  system.GetJitInterface().JitBlockLogDump(Core::CPUThreadGuard{system}, dump.GetHandle());

  // The confirmation can be turned off for the rest of the session with Ignore.
  if (m_quiet_block_dump)
    return;
  const int answer =
      ModalMessageBox::information(this, tr("Success"), tr("Wrote to \"%1\".").arg(shown_path),
                                   QMessageBox::Ok | QMessageBox::Ignore);
  if (answer == QMessageBox::Ignore)
    m_quiet_block_dump = true;
}

void MenuBar::SearchForInstruction()
{
  bool accepted = false;
  const QString wanted =
      QInputDialog::getText(this, tr("Search instruction"), tr("Instruction:"), QLineEdit::Normal,
                            QString{}, &accepted, Qt::WindowCloseButtonHint);
  if (!accepted)
    return;

  auto& system = TheSystem();
  const u32 ram_end = Memory::MEM1_BASE_ADDR + system.GetMemory().GetRamSizeReal();
  const std::string mnemonic = wanted.toStdString();
  const Core::CPUThreadGuard guard(system);

  // Every word of MEM1 is decoded; matches are reported in the log.
  bool any = false;
  for (u32 address = Memory::MEM1_BASE_ADDR; address < ram_end; address += 4)
  {
    const u32 word = PowerPC::MMU::HostRead<u32>(guard, address);
    if (PPCTables::GetInstructionName(word, address) != mnemonic)
      continue;
    NOTICE_LOG_FMT(POWERPC, "Found {} at {:08x}", mnemonic, address);
    any = true;
  }
  if (!any)
    NOTICE_LOG_FMT(POWERPC, "Opcode {} not found", mnemonic);
}

// --- Symbols -----------------------------------------------------------------------------------

void MenuBar::ClearSymbols()
{
  const auto answer = ModalMessageBox::warning(
      this, tr("Confirmation"), tr("Do you want to clear the list of symbol names?"),
      QMessageBox::Yes | QMessageBox::Cancel);
  if (answer == QMessageBox::Cancel)
    return;

  TheSystem().GetPPCSymbolDB().Clear();
  AnnounceSymbolsChanged();
}

void MenuBar::GenerateSymbolsFromAddress()
{
  auto& system = TheSystem();
  const u32 ram_end = Memory::MEM1_BASE_ADDR + system.GetMemory().GetRamSizeReal();

  const Core::CPUThreadGuard guard(system);
  PPCAnalyst::FindFunctions(guard, Memory::MEM1_BASE_ADDR, ram_end, &system.GetPPCSymbolDB());
  AnnounceSymbolsChanged();
}

void MenuBar::GenerateSymbolsFromSignatureDB()
{
  auto& system = TheSystem();
  auto& symbols = system.GetPPCSymbolDB();
  const u32 ram_end = Memory::MEM1_BASE_ADDR + system.GetMemory().GetRamSizeReal();
  const QString database_name = QString::fromStdString(TOTALDB);

  const Core::CPUThreadGuard guard(system);
  PPCAnalyst::FindFunctions(guard, Memory::MEM1_BASE_ADDR, ram_end, &symbols);

  SignatureDB database(SignatureDB::HandlerType::DSY);
  if (database.Load(File::GetSysDirectory() + TOTALDB))
  {
    database.Apply(guard, &symbols);
    ModalMessageBox::information(this, tr("Information"),
                                 tr("Generated symbol names from '%1'").arg(database_name));
    database.List();
  }
  else
  {
    ModalMessageBox::critical(this, tr("Error"),
                              tr("'%1' not found, no symbol names generated").arg(database_name));
  }

  AnnounceSymbolsChanged();
}

void MenuBar::LoadRSOChain(u32 address, const QString& address_text)
{
  auto& system = TheSystem();
  const Core::CPUThreadGuard guard(system);

  RSOChainView chain;
  if (!chain.Load(guard, address))
  {
    ModalMessageBox::warning(this, tr("Error"),
                             tr("Failed to load RSO module at %1").arg(address_text));
    return;
  }
  chain.Apply(guard, &system.GetPPCSymbolDB());
  AnnounceSymbolsChanged();
}

void MenuBar::GenerateSymbolsFromRSO()
{
  // i18n: RSO is the format of the Wii's relocatable modules (comparable to DLL files).
  const int auto_detect =
      ModalMessageBox::question(this, tr("RSO auto-detection"), tr("Auto-detect RSO modules?"));
  if (auto_detect == QMessageBox::Yes)
  {
    GenerateSymbolsFromRSOAuto();
    return;
  }

  const QString typed =
      QInputDialog::getText(this, tr("Input"), tr("Enter the RSO module address:"),
                            QLineEdit::Normal, QString{}, nullptr, Qt::WindowCloseButtonHint);
  bool is_hex = false;
  const uint address = typed.toUInt(&is_hex, 16);
  if (!is_hex)
  {
    ModalMessageBox::warning(this, tr("Error"), tr("Invalid RSO module address: %1").arg(typed));
    return;
  }

  LoadRSOChain(static_cast<u32>(address), typed);
}

void MenuBar::GenerateSymbolsFromRSOAuto()
{
  ParallelProgressDialog progress(tr("Modules found: %1").arg(0), tr("Cancel"), 0, 0, this);
  QProgressDialog* const dialog = progress.GetRaw();
  dialog->setWindowTitle(tr("Detecting RSO Modules"));
  dialog->setMinimumDuration(10 * 1000);
  dialog->setWindowModality(Qt::WindowModal);

  // The scan runs on a worker thread; the dialog's event loop ends when it calls Reset().
  std::future<RSOVector> scan = std::async(std::launch::async, [this, &progress] {
    progress.SetValue(0);
    RSOVector found = DetectRSOModules(progress);
    progress.Reset();
    return found;
  });
  dialog->exec();
  const RSOVector modules = scan.get();

  QStringList choices;
  for (const RSOPairEntry& module : modules)
  {
    choices.append(QStringLiteral("%1 %2").arg(QString::number(module.first, 16),
                                               QString::fromStdString(module.second)));
  }
  if (choices.isEmpty())
  {
    ModalMessageBox::warning(this, tr("Error"), tr("Unable to auto-detect RSO module"));
    return;
  }

  bool picked = false;
  const QString choice =
      QInputDialog::getItem(this, tr("Input"), tr("Select the RSO module address:"), choices, 0,
                            false, &picked, Qt::WindowCloseButtonHint);
  if (!picked)
    return;

  // Each choice starts with the header address in hex.
  const u32 address = choice.left(choice.indexOf(QLatin1Char(' '))).toUInt(nullptr, 16);
  LoadRSOChain(address, QString::number(address));
}

// Looks for loaded RSO modules by their file names. A module's name (such as "xyz.elf") is stored
// in memory with the module, and the module header has a (name offset, name length) pair pointing
// at it; the header begins 16 bytes before that pair.
RSOVector MenuBar::DetectRSOModules(ParallelProgressDialog& progress)
{
  const Core::CPUThreadGuard guard(TheSystem());
  const AddressSpace::Accessors* const memory =
      AddressSpace::GetAccessors(AddressSpace::Type::Effective);

  // Bytes before the extension that can belong to the name: printable, at most 260 of them.
  const auto printable_run_before = [&guard](u32 address) {
    constexpr u32 LONGEST_NAME = 260;
    u32 length = 0;
    while (length < LONGEST_NAME &&
           std::isprint(PowerPC::MMU::HostRead<u8>(guard, address - (length + 1))))
    {
      ++length;
    }
    return length;
  };

  RSOVector modules;
  for (const std::string_view extension : {std::string_view(".elf"), std::string_view(".plf")})
  {
    u32 search_from = 0;
    while (!progress.WasCanceled())
    {
      // The extension, with its terminating NUL.
      const std::optional<u32> extension_at =
          memory->Search(guard, search_from, reinterpret_cast<const u8*>(extension.data()),
                         extension.size() + 1, true);
      if (!extension_at)
        break;
      search_from = *extension_at + 1;

      if (progress.WasCanceled())
        return modules;

      // Try each possible start of the name, longest first: a candidate start is right if some
      // word in memory points at it and is followed by the matching name length.
      const u32 longest = printable_run_before(*extension_at);
      std::optional<u32> name_field;
      u32 name_length = 0;
      for (u32 skipped = 0; skipped < longest; ++skipped)
      {
        if (progress.WasCanceled())
          return modules;

        const u32 name_start = *extension_at - longest + skipped;
        const std::array<u8, 4> pointer_bytes{
            static_cast<u8>(name_start >> 24), static_cast<u8>(name_start >> 16),
            static_cast<u8>(name_start >> 8), static_cast<u8>(name_start)};
        const std::optional<u32> pointer_at = memory->Search(
            guard, name_start, pointer_bytes.data(), pointer_bytes.size(), false);
        if (!pointer_at)
          continue;

        name_length = memory->ReadU32(guard, *pointer_at + 4);
        if (name_length == longest - skipped + extension.length())
        {
          name_field = pointer_at;
          break;
        }
      }
      if (!name_field)
        continue;

      const u32 name_address = memory->ReadU32(guard, *name_field);
      modules.emplace_back(*name_field - 16,
                           PowerPC::MMU::HostGetString(guard, name_address, name_length));
      progress.SetLabelText(tr("Modules found: %1").arg(modules.size()));
    }
    if (progress.WasCanceled())
      return modules;
  }

  return modules;
}

void MenuBar::LoadSymbolMap()
{
  auto& system = TheSystem();
  auto& symbols = system.GetPPCSymbolDB();

  std::string existing_map;
  std::string writable_map;
  if (PPCSymbolDB::FindMapFile(&existing_map, &writable_map))
  {
    const QString path = QString::fromStdString(existing_map);
    if (!TryLoadMapFile(path))
      return;
    ModalMessageBox::information(this, tr("Information"), tr("Loaded symbols from '%1'").arg(path));
  }
  else
  {
    // No map for this game: scan for functions (above the usual OS area) and name what the
    // signature database recognises.
    symbols.Clear();
    {
      const u32 ram_end = Memory::MEM1_BASE_ADDR + system.GetMemory().GetRamSizeReal();
      const Core::CPUThreadGuard guard(system);
      PPCAnalyst::FindFunctions(guard, Memory::MEM1_BASE_ADDR + 0x1300000, ram_end, &symbols);
      SignatureDB database(SignatureDB::HandlerType::DSY);
      if (database.Load(File::GetSysDirectory() + TOTALDB))
        database.Apply(guard, &symbols);
    }
    ModalMessageBox::warning(this, tr("Warning"),
                             tr("'%1' not found, scanning for common functions instead")
                                 .arg(QString::fromStdString(writable_map)));
  }

  HLE::PatchFunctions(system);
  AnnounceSymbolsChanged();
}

void MenuBar::LoadMapFileChosenByUser(bool bad)
{
  const QString path = DolphinFileDialog::getOpenFileName(
      this, tr("Load Map File"), QString::fromStdString(File::GetUserPath(D_MAPS_IDX)),
      tr("Dolphin Map File (*.map)"));
  if (path.isEmpty() || !TryLoadMapFile(path, bad))
    return;

  HLE::PatchFunctions(TheSystem());
  AnnounceSymbolsChanged();
}

void MenuBar::SaveSymbolMap()
{
  std::string existing_map;
  std::string writable_map;
  PPCSymbolDB::FindMapFile(&existing_map, &writable_map);
  TrySaveSymbolMap(QString::fromStdString(writable_map));
}

void MenuBar::SaveSymbolMapAs()
{
  const std::string& game_id = SConfig::GetInstance().m_debugger_game_id;
  const QString suggested =
      QString::fromStdString(File::GetUserPath(D_MAPS_IDX) + "/" + game_id + ".map");
  const QString path = DolphinFileDialog::getSaveFileName(this, tr("Save Map File"), suggested,
                                                          tr("Dolphin Map File (*.map)"));
  if (!path.isEmpty())
    TrySaveSymbolMap(path);
}

void MenuBar::SaveCode()
{
  std::string existing_map;
  std::string writable_map;
  PPCSymbolDB::FindMapFile(&existing_map, &writable_map);

  // Next to the symbol map: "<map name without extension>_code.map".
  const std::string path = writable_map.substr(0, writable_map.find_last_of('.')) + "_code.map";

  auto& system = TheSystem();
  if (system.GetPPCSymbolDB().SaveCodeMap(Core::CPUThreadGuard{system}, path))
    return;
  ModalMessageBox::warning(
      this, tr("Error"),
      tr("Failed to save code map to path '%1'").arg(QString::fromStdString(path)));
}

bool MenuBar::TryLoadMapFile(const QString& path, const bool bad)
{
  auto& system = TheSystem();
  if (system.GetPPCSymbolDB().LoadMap(Core::CPUThreadGuard{system}, path.toStdString(), bad))
    return true;

  ModalMessageBox::warning(this, tr("Error"), tr("Failed to load map file '%1'").arg(path));
  return false;
}

void MenuBar::TrySaveSymbolMap(const QString& path)
{
  if (!TheSystem().GetPPCSymbolDB().SaveSymbolMap(path.toStdString()))
  {
    ModalMessageBox::warning(this, tr("Error"),
                             tr("Failed to save symbol map to path '%1'").arg(path));
  }
}

void MenuBar::CreateSignatureFile()
{
  const QString prefix = QInputDialog::getText(
      this, tr("Input"), tr("Only export symbols with prefix:\n(Blank for all symbols)"),
      QLineEdit::Normal, QString{}, nullptr, Qt::WindowCloseButtonHint);

  const QString target = DolphinFileDialog::getSaveFileName(
      this, tr("Save Signature File"), QDir::homePath(), SignatureFileFilter());
  if (target.isEmpty())
    return;

  const std::string target_path = target.toStdString();
  SignatureDB database(target_path);
  database.Populate(&TheSystem().GetPPCSymbolDB(), prefix.toStdString());
  if (!database.Save(target_path))
  {
    ModalMessageBox::warning(this, tr("Error"),
                             tr("Failed to save signature file '%1'").arg(target));
    return;
  }
  database.List();
}

void MenuBar::AppendSignatureFile()
{
  const QString prefix = QInputDialog::getText(
      this, tr("Input"), tr("Only append symbols with prefix:\n(Blank for all symbols)"),
      QLineEdit::Normal, QString{}, nullptr, Qt::WindowCloseButtonHint);

  const QString target = DolphinFileDialog::getSaveFileName(
      this, tr("Append Signature To"), QDir::homePath(), SignatureFileFilter());
  if (target.isEmpty())
    return;

  // Current symbols first, then whatever the file already holds, written back together.
  const std::string target_path = target.toStdString();
  SignatureDB database(target_path);
  database.Populate(&TheSystem().GetPPCSymbolDB(), prefix.toStdString());
  database.List();
  database.Load(target_path);
  if (!database.Save(target_path))
  {
    ModalMessageBox::warning(this, tr("Error"),
                             tr("Failed to append to signature file '%1'").arg(target));
    return;
  }
  database.List();
}

void MenuBar::ApplySignatureFile()
{
  const QString source = DolphinFileDialog::getOpenFileName(
      this, tr("Apply Signature File"), QDir::homePath(), SignatureFileFilter());
  if (source.isEmpty())
    return;

  auto& system = TheSystem();
  const std::string source_path = source.toStdString();
  SignatureDB database(source_path);
  database.Load(source_path);
  database.Apply(Core::CPUThreadGuard{system}, &system.GetPPCSymbolDB());
  database.List();
  HLE::PatchFunctions(system);
  AnnounceSymbolsChanged();
}

void MenuBar::CombineSignatureFiles()
{
  // Entries of the first file win over those of the second.
  const QString primary = DolphinFileDialog::getOpenFileName(
      this, tr("Choose Priority Input File"), QDir::homePath(), SignatureFileFilter());
  if (primary.isEmpty())
    return;
  const QString secondary = DolphinFileDialog::getOpenFileName(
      this, tr("Choose Secondary Input File"), QDir::homePath(), SignatureFileFilter());
  if (secondary.isEmpty())
    return;
  const QString output = DolphinFileDialog::getSaveFileName(
      this, tr("Save Combined Output File As"), QDir::homePath(), SignatureFileFilter());
  if (output.isEmpty())
    return;

  const std::string primary_path = primary.toStdString();
  SignatureDB database(primary_path);
  database.Load(primary_path);
  database.Load(secondary.toStdString());
  if (!database.Save(output.toStdString()))
  {
    ModalMessageBox::warning(this, tr("Error"),
                             tr("Failed to save to signature file '%1'").arg(output));
    return;
  }
  database.List();
}
