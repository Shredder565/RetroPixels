// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/MainWindow.h"

#include <algorithm>
#include <cstdio>
#include <future>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>

#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QIcon>
#include <QMimeData>
#include <QStackedWidget>
#include <QStyleHints>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <fmt/format.h>

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
#include <signal.h>

#include "QtUtils/SignalDaemon.h"
#endif

#ifndef _WIN32
#include <qpa/qplatformnativeinterface.h>
#endif

#if defined(QT_DBUS_LIB)
#include <QDBusInterface>
#include <QDBusReply>
#endif

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/ScopeGuard.h"
#include "Common/Version.h"
#include "Common/WindowSystemInfo.h"

#include "Core/AchievementManager.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/CommonTitles.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/FreeLookManager.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HotkeyManager.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/Movie.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayServer.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"

#include "DiscIO/DirectoryBlob.h"
#include "DiscIO/NANDImporter.h"
#include "DiscIO/RiivolutionPatcher.h"

#include "DolphinQt/AboutDialog.h"
#include "DolphinQt/Achievements/AchievementsWindow.h"
#include "DolphinQt/CheatsManager.h"
#include "DolphinQt/Config/FreeLookWindow.h"
#include "DolphinQt/Config/LogConfigWidget.h"
#include "DolphinQt/Config/LogWidget.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/Config/SettingsWindow.h"
#include "DolphinQt/Debugger/AssemblerWidget.h"
#include "DolphinQt/Debugger/BreakpointWidget.h"
#include "DolphinQt/Debugger/CodeViewWidget.h"
#include "DolphinQt/Debugger/CodeWidget.h"
#include "DolphinQt/Debugger/JITWidget.h"
#include "DolphinQt/Debugger/MemoryWidget.h"
#include "DolphinQt/Debugger/NetworkWidget.h"
#include "DolphinQt/Debugger/RegisterWidget.h"
#include "DolphinQt/Debugger/ThreadWidget.h"
#include "DolphinQt/Debugger/WatchWidget.h"
#include "DolphinQt/DiscordHandler.h"
#include "DolphinQt/EmulatedUSB/LogitechMicWindow.h"
#include "DolphinQt/EmulatedUSB/WiiSpeakWindow.h"
#include "DolphinQt/FIFO/FIFOPlayerWindow.h"
#include "DolphinQt/GCMemcardManager.h"
#include "DolphinQt/GameCount.h"
#include "DolphinQt/GameList/GameList.h"
#include "DolphinQt/Host.h"
#include "DolphinQt/HotkeyScheduler.h"
#include "DolphinQt/InfinityBase/InfinityBaseWindow.h"
#include "DolphinQt/MenuBar.h"
#include "DolphinQt/NKitWarningDialog.h"
#include "DolphinQt/NetPlay/NetPlayBrowser.h"
#include "DolphinQt/NetPlay/NetPlayDialog.h"
#include "DolphinQt/NetPlay/NetPlaySetupDialog.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/FileOpenEventFilter.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/ParallelProgressDialog.h"
#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QtUtils/RunOnObject.h"
#include "DolphinQt/QtUtils/WindowActivationEventFilter.h"
#include "DolphinQt/RenderWidget.h"
#include "DolphinQt/ResourcePackManager.h"
#include "DolphinQt/Resources.h"
#include "DolphinQt/RiivolutionBootWidget.h"
#include "DolphinQt/SearchBar.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/SkylanderPortal/SkylanderPortalWindow.h"
#include "DolphinQt/TAS/GBATASInputWindow.h"
#include "DolphinQt/TAS/GCTASInputWindow.h"
#include "DolphinQt/TAS/WiiTASInputWindow.h"
#include "DolphinQt/ToolBar.h"
#include "DolphinQt/WiiUpdate.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"

#include "UICommon/DiscordPresence.h"
#include "UICommon/GameFile.h"
#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/ResourcePack/ResourcePack.h"
#include "UICommon/UICommon.h"

#include "VideoCommon/NetPlayChatUI.h"

#ifdef HAVE_XRANDR
#include "UICommon/X11Utils.h"
// X11/X.h defines None, which collides with WiimoteSource::None below.
#undef None
#endif

namespace
{
// PixelU: the main window's title whenever no game is drawing into it.
QString IdleWindowTitle()
{
  return QStringLiteral("PixelU Wii %1")
      .arg(QString::fromStdString(Common::GetScmDescStr()));
}

// Maps the Qt platform plugin in use to the window system the video backends must talk to.
WindowSystemType DetectWindowSystem()
{
  struct Platform
  {
    const char* plugin;
    WindowSystemType type;
  };
  static constexpr Platform known_platforms[] = {
      {"windows", WindowSystemType::Windows}, {"cocoa", WindowSystemType::MacOS},
      {"xcb", WindowSystemType::X11},         {"wayland", WindowSystemType::Wayland},
      {"haiku", WindowSystemType::Haiku},
  };

  const QString plugin = QGuiApplication::platformName();
  for (const Platform& platform : known_platforms)
  {
    if (plugin == QLatin1String(platform.plugin))
      return platform.type;
  }

  ModalMessageBox::critical(nullptr, QStringLiteral("Error"),
                            QStringLiteral("Unknown Qt platform: %1").arg(plugin));
  return WindowSystemType::Headless;
}

// Describes a native window (or none) in the form the video backends and input code take.
WindowSystemInfo DescribeWindow(QWindow* window)
{
  WindowSystemInfo info;
  info.type = DetectWindowSystem();

  const auto window_id = [window]() -> void* {
    return window != nullptr ? reinterpret_cast<void*>(window->winId()) : nullptr;
  };
#if defined(_WIN32) || defined(__APPLE__) || defined(__HAIKU__)
  // The Qt build used here does not ship the private native interface; the window id is enough.
  info.render_window = window_id();
#else
  QPlatformNativeInterface* const native = QGuiApplication::platformNativeInterface();
  info.display_connection = native->nativeResourceForWindow("display", window);
  if (info.type != WindowSystemType::Wayland)
    info.render_window = window_id();
  else if (window != nullptr)
    info.render_window = native->nativeResourceForWindow("surface", window);
  else
    info.render_window = nullptr;
#endif
  info.render_surface = info.render_window;
  info.render_surface_scale =
      window != nullptr ? static_cast<float>(window->devicePixelRatio()) : 1.0f;
  return info;
}

std::vector<std::string> ToStdStrings(const QStringList& strings)
{
  std::vector<std::string> converted;
  converted.reserve(static_cast<size_t>(strings.size()));
  std::transform(strings.cbegin(), strings.cend(), std::back_inserter(converted),
                 [](const QString& text) { return text.toStdString(); });
  return converted;
}

void BringToFront(QWidget* window)
{
  window->show();
  window->raise();
  window->activateWindow();
}

// Shows a window that is created on first use; make() builds it the first time.
template <typename Window, typename Make>
Window* Summon(Window*& window, Make&& make)
{
  if (window == nullptr)
    window = make();
  BringToFront(window);
  return window;
}

void OpenUserDirectory(unsigned int directory_index)
{
  const std::string& path = File::GetUserPath(directory_index);
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(path)));
}

// Global hotkeys stay off while the given configuration window is in front, so pressing a key
// to bind it does not also trigger the hotkey it is currently bound to.
void SuspendHotkeysWhileActive(QWidget* window)
{
  auto* const watcher = new WindowActivationEventFilter(window);
  window->installEventFilter(watcher);

  QObject::connect(watcher, &WindowActivationEventFilter::windowDeactivated, watcher,
                   [] { HotkeyManagerEmu::Enable(true); });
  QObject::connect(watcher, &WindowActivationEventFilter::windowActivated, watcher,
                   [] { HotkeyManagerEmu::Enable(false); });
}

// Creates a widget owned by parent; the parent is the constructor's last argument.
template <typename Widget, typename... Args>
Widget* MakeChild(QWidget* parent, Args&&... args)
{
  return new Widget(std::forward<Args>(args)..., parent);
}

std::unique_ptr<BootSessionData> SessionWithState(const std::optional<std::string>& savestate)
{
  return std::make_unique<BootSessionData>(savestate, DeleteSavestateAfterBoot::No);
}

#if defined(QT_DBUS_LIB)
// Linux desktops (GNOME, KDE, Xfce, ...) keep the screen on while a program holds an inhibition
// of org.freedesktop.ScreenSaver on the session bus. Without a session bus or that service this
// does nothing.
void InhibitDesktopScreenSaver(bool inhibit)
{
  static std::optional<uint> s_cookie;
  if (inhibit == s_cookie.has_value())
    return;

  const QString service = QStringLiteral("org.freedesktop.ScreenSaver");
  QDBusInterface saver(service, QStringLiteral("/org/freedesktop/ScreenSaver"), service);
  if (!saver.isValid())
    return;
  if (inhibit)
  {
    const QDBusReply<uint> reply =
        saver.call(QStringLiteral("Inhibit"), QStringLiteral("PixelU Wii"),
                   QStringLiteral("A game is running"));
    if (reply.isValid())
      s_cookie = reply.value();
  }
  else
  {
    saver.call(QStringLiteral("UnInhibit"), *s_cookie);
    s_cookie.reset();
  }
}
#endif

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
// SIGINT and SIGTERM are handed to SignalDaemon, which turns them into a Qt signal on the GUI
// thread. A second signal of the same kind gets the default action and ends the process.
void RouteTerminationSignals()
{
  struct sigaction action = {};
  action.sa_handler = &SignalDaemon::HandleInterrupt;
  sigemptyset(&action.sa_mask);
  action.sa_flags = static_cast<int>(SA_RESTART | SA_RESETHAND);
  for (const int signal_number : {SIGINT, SIGTERM})
    sigaction(signal_number, &action, nullptr);
}
#endif
}  // namespace

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
void MainWindow::OnSignal()
{
  close();
}
#endif

MainWindow::MainWindow(Core::System& system, std::unique_ptr<BootParameters> boot_parameters,
                       const std::string& movie_path)
    : QMainWindow(nullptr), m_system(system)
{
  setWindowTitle(IdleWindowTitle());
  setWindowIcon(Resources::GetAppIcon());
  setUnifiedTitleAndToolBarOnMac(true);
  setAcceptDrops(true);
  setAttribute(Qt::WA_NativeWindow);

  CreateComponents();

  // Wire up the parts (the order matters: the stack takes over widgets the others set up).
  using WiringStep = void (MainWindow::*)();
  for (const WiringStep step :
       {&MainWindow::ConnectGameList, &MainWindow::ConnectHost, &MainWindow::ConnectToolBar,
        &MainWindow::ConnectRenderWidget, &MainWindow::ConnectStack, &MainWindow::ConnectMenuBar})
  {
    (this->*step)();
  }

  QSettings& qsettings = Settings::GetQSettings();
  restoreState(qsettings.value(QStringLiteral("mainwindow/state")).toByteArray());
  restoreGeometry(qsettings.value(QStringLiteral("mainwindow/geometry")).toByteArray());
  const bool batch_mode = Settings::Instance().IsBatchModeEnabled();
  if (!batch_mode)
    show();

  InitControllers();
  ConnectHotkeys();

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
    Settings::Instance().ApplyStyle();
    if (m_skylander_window != nullptr)
      m_skylander_window->RefreshList();
  });
#endif

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_game_list, &GameList::OpenAchievementSettings, this,
          &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS

  InitCoreCallbacks();
  NetPlayInit();

#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementManager& achievements = AchievementManager::GetInstance();
  achievements.Init(reinterpret_cast<void*>(winId()));
  if (achievements.IsHardcoreModeActive())
    Settings::Instance().SetDebugModeEnabled(false);
  // Any config change may switch hardcore mode on or off (RA_ENABLED as well as
  // RA_HARDCORE_ENABLED), so recheck it on the GUI thread after every change.
  m_config_changed_callback_id = Config::AddConfigChangedCallback(
      [this] { QueueOnObject(this, [this] { OnHardcoreChanged(); }); });
  if (Config::Get(Config::RA_HARDCORE_ENABLED))
    OnHardcoreChanged();
#endif  // USE_RETRO_ACHIEVEMENTS

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
  auto* const signal_daemon = new SignalDaemon(this);
  connect(signal_daemon, &SignalDaemon::InterruptReceived, this, &MainWindow::OnSignal);
  RouteTerminationSignals();
#endif

  m_pending_boot = std::move(boot_parameters);
  if (m_pending_boot != nullptr && !movie_path.empty())
  {
    // A movie given on the command line may start from a save state of its own.
    std::optional<std::string> movie_savestate;
    if (m_system.GetMovie().PlayInput(movie_path, &movie_savestate))
    {
      m_pending_boot->boot_session_data.SetSavestateData(std::move(movie_savestate),
                                                         DeleteSavestateAfterBoot::No);
      emit RecordingStatusChanged(true);
    }
  }

  const int last_slot = Settings::Instance().GetStateSlot();
  m_state_slot = static_cast<u32>(std::clamp(last_slot, 1, static_cast<int>(State::NUM_STATES)));

  m_render_widget_geometry =
      qsettings.value(QStringLiteral("renderwidget/geometry")).toByteArray();

  // restoreState() can bring back docks that the current settings say should be hidden.
  Settings::Instance().RefreshWidgetVisibility();

  if (!CheckResourcePacks())
    return;

  Host::GetInstance()->SetMainWindowHandle(reinterpret_cast<void*>(winId()));

  if (m_pending_boot != nullptr)
  {
    StartGame(std::move(m_pending_boot));
    m_pending_boot.reset();
  }
}

MainWindow::~MainWindow()
{
  // NetPlay threads call back into the GUI, so they have to go before anything else does.
  Settings& settings = Settings::Instance();
  settings.ResetNetPlayClient();
  settings.ResetNetPlayServer();

#ifdef USE_RETRO_ACHIEVEMENTS
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
  AchievementManager::GetInstance().Shutdown();
#endif  // USE_RETRO_ACHIEVEMENTS

  delete m_render_widget;
  delete m_netplay_dialog;

  for (int port = 0; port < PORT_COUNT; ++port)
  {
    const auto index = static_cast<size_t>(port);
    delete m_gc_tas_input_windows[index];
    delete m_gba_tas_input_windows[index];
    delete m_wii_tas_input_windows[index];
  }

  ShutdownControllers();

  QSettings& qsettings = Settings::GetQSettings();
  if (!settings.IsBatchModeEnabled())
  {
    qsettings.setValue(QStringLiteral("mainwindow/state"), saveState());
    qsettings.setValue(QStringLiteral("mainwindow/geometry"), saveGeometry());
  }
  qsettings.setValue(QStringLiteral("renderwidget/geometry"), m_render_widget_geometry);

  Config::Save();
}

WindowSystemInfo MainWindow::GetWindowSystemInfo() const
{
  return DescribeWindow(m_render_widget->windowHandle());
}

bool MainWindow::CheckResourcePacks()
{
  if (!ResourcePack::Init())
  {
    ModalMessageBox::critical(this, tr("Error"),
                              tr("Error occurred while loading some texture packs"));
  }

  const auto& packs = ResourcePack::GetPacks();
  const auto broken = std::find_if(packs.begin(), packs.end(),
                                   [](const auto& pack) { return !pack.IsValid(); });
  if (broken == packs.end())
    return true;

  ModalMessageBox::critical(this, tr("Error"),
                            tr("Invalid Pack %1 provided: %2")
                                .arg(QString::fromStdString(broken->GetPath()))
                                .arg(QString::fromStdString(broken->GetError())));
  return false;
}

void MainWindow::InitControllers()
{
  if (g_controller_interface.IsInit())
    return;

  UICommon::InitControllers(DescribeWindow(windowHandle()));

  m_hotkey_scheduler = new HotkeyScheduler();
  m_hotkey_scheduler->Start();

  // Each input config is loaded and written straight back once, so that defaults for newly
  // added settings end up in the files before anything else reads them.
  struct InputConfigFile
  {
    void (*load)();
    InputConfig* (*get)();
  };
  static constexpr InputConfigFile config_files[] = {
      {&Wiimote::LoadConfig, &Wiimote::GetConfig},
      {&Pad::LoadConfig, &Pad::GetConfig},
      {&Pad::LoadGBAConfig, &Pad::GetGBAConfig},
      {&Keyboard::LoadConfig, &Keyboard::GetConfig},
      {&FreeLook::LoadInputConfig, &FreeLook::GetInputConfig},
  };
  for (const InputConfigFile& file : config_files)
  {
    file.load();
    file.get()->SaveConfig();
  }
}

void MainWindow::ShutdownControllers()
{
  m_hotkey_scheduler->Stop();
  Settings::Instance().UnregisterDevicesChangedCallback();
  UICommon::ShutdownControllers();
  m_hotkey_scheduler->deleteLater();
}

void MainWindow::InitCoreCallbacks()
{
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    if (state == Core::State::Uninitialized)
      OnStopComplete();

    if (state != Core::State::Running || !m_fullscreen_requested)
      return;
    FullScreen();
    m_fullscreen_requested = false;
  });

  // Closing either window goes through RequestStop (see eventFilter).
  installEventFilter(this);
  m_render_widget->installEventFilter(this);

  // Files handed to the application by the OS (macOS "open with", for example).
  auto* const open_events = new FileOpenEventFilter(QGuiApplication::instance());
  connect(open_events, &FileOpenEventFilter::fileOpened, this, [this](const QString& file) {
    StartGame(BootParameters::GenerateFromFile(file.toStdString()));
  });
}

void MainWindow::CreateComponents()
{
  m_menu_bar = MakeChild<MenuBar>(this);
  m_tool_bar = MakeChild<ToolBar>(this);
  m_search_bar = MakeChild<SearchBar>(this);
  m_game_count = MakeChild<GameCount>(this);
  m_game_list = MakeChild<GameList>(this);
  m_stack = MakeChild<QStackedWidget>(this);
  // The render widget is a top-level window of its own until a game renders to the main window.
  m_render_widget = new RenderWidget;

  for (int port = 0; port < PORT_COUNT; ++port)
  {
    const auto index = static_cast<size_t>(port);
    m_gc_tas_input_windows[index] = new GCTASInputWindow(nullptr, port);
    m_gba_tas_input_windows[index] = new GBATASInputWindow(nullptr, port);
    m_wii_tas_input_windows[index] = new WiiTASInputWindow(nullptr, port);
  }

  CreateDebuggerWidgets();
  LinkDebuggerWidgets();
}

void MainWindow::CreateDebuggerWidgets()
{
  m_jit_widget = MakeChild<JITWidget>(this, m_system);
  m_log_widget = MakeChild<LogWidget>(this);
  m_log_config_widget = MakeChild<LogConfigWidget>(this);
  m_memory_widget = MakeChild<MemoryWidget>(this, m_system);
  m_network_widget = MakeChild<NetworkWidget>(this);
  m_register_widget = MakeChild<RegisterWidget>(this);
  m_thread_widget = MakeChild<ThreadWidget>(this);
  m_watch_widget = MakeChild<WatchWidget>(this);
  m_breakpoint_widget = MakeChild<BreakpointWidget>(this);
  m_code_widget = MakeChild<CodeWidget>(this);
  m_assembler_widget = MakeChild<AssemblerWidget>(this);
}

// Lets the debugger docks send each other to an address: "show this in the memory view",
// "set a breakpoint here", and so on.
void MainWindow::LinkDebuggerWidgets()
{
  const auto add_watch = [this](const QString& name, u32 address) {
    m_watch_widget->AddWatch(name, address);
  };
  const auto add_breakpoint = [this](u32 address) { m_breakpoint_widget->AddBP(address); };
  const auto add_memory_breakpoint = [this](u32 address) {
    m_breakpoint_widget->AddAddressMBP(address);
  };
  const auto show_in_memory = [this](u32 address) { m_memory_widget->SetAddress(address); };
  const auto show_in_code = [this](u32 address) {
    m_code_widget->SetAddress(address, CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
  };

  connect(m_jit_widget, &JITWidget::SetCodeAddress, m_code_widget, &CodeWidget::OnSetCodeAddress);

  connect(m_watch_widget, &WatchWidget::RequestMemoryBreakpoint, this, add_memory_breakpoint);
  connect(m_watch_widget, &WatchWidget::ShowMemory, m_memory_widget, &MemoryWidget::SetAddress);

  connect(m_register_widget, &RegisterWidget::RequestMemoryBreakpoint, this,
          add_memory_breakpoint);
  connect(m_register_widget, &RegisterWidget::RequestWatch, this, add_watch);
  connect(m_register_widget, &RegisterWidget::RequestViewInMemory, this, show_in_memory);
  connect(m_register_widget, &RegisterWidget::RequestViewInCode, this, show_in_code);

  connect(m_thread_widget, &ThreadWidget::RequestBreakpoint, this, add_breakpoint);
  connect(m_thread_widget, &ThreadWidget::RequestMemoryBreakpoint, this, add_memory_breakpoint);
  connect(m_thread_widget, &ThreadWidget::RequestWatch, this, add_watch);
  connect(m_thread_widget, &ThreadWidget::RequestViewInMemory, this, show_in_memory);
  connect(m_thread_widget, &ThreadWidget::RequestViewInCode, this, show_in_code);

  connect(m_code_widget, &CodeWidget::RequestPPCComparison, m_jit_widget,
          &JITWidget::OnRequestPPCComparison);
  connect(m_code_widget, &CodeWidget::ShowMemory, m_memory_widget, &MemoryWidget::SetAddress);

  connect(m_memory_widget, &MemoryWidget::ShowCode, m_code_widget, show_in_code);
  connect(m_memory_widget, &MemoryWidget::RequestWatch, this, add_watch);

  // A breakpoint's code can only be shown while the CPU is stopped.
  connect(m_breakpoint_widget, &BreakpointWidget::ShowCode, this,
          [this, show_in_code](u32 address) {
            if (Core::GetState(m_system) == Core::State::Paused)
              show_in_code(address);
          });
  connect(m_breakpoint_widget, &BreakpointWidget::ShowMemory, m_memory_widget,
          &MemoryWidget::SetAddress);
}

void MainWindow::ConnectMenuBar()
{
  setMenuBar(m_menu_bar);

  const auto handle = [this](auto signal, auto slot) { connect(m_menu_bar, signal, this, slot); };
  const auto to_list = [this](auto signal, auto slot) {
    connect(m_menu_bar, signal, m_game_list, slot);
  };
  const auto play = [this] { Play(); };

  // File menu
  handle(&MenuBar::Open, &MainWindow::Open);
  handle(&MenuBar::Exit, &MainWindow::close);
  handle(&MenuBar::EjectDisc, &MainWindow::EjectDisc);
  handle(&MenuBar::ChangeDisc, &MainWindow::ChangeDisc);
  handle(&MenuBar::OpenUserFolder, &MainWindow::OpenUserFolder);
  handle(&MenuBar::OpenConfigFolder, &MainWindow::OpenConfigFolder);
  handle(&MenuBar::OpenCacheFolder, &MainWindow::OpenCacheFolder);

  // Emulation menu
  handle(&MenuBar::Pause, &MainWindow::Pause);
  handle(&MenuBar::Play, play);
  handle(&MenuBar::Stop, &MainWindow::RequestStop);
  handle(&MenuBar::Reset, &MainWindow::Reset);
  handle(&MenuBar::Fullscreen, &MainWindow::FullScreen);
  handle(&MenuBar::FrameAdvance, &MainWindow::FrameAdvance);
  handle(&MenuBar::Screenshot, &MainWindow::ScreenShot);
  handle(&MenuBar::StateLoad, &MainWindow::StateLoad);
  handle(&MenuBar::StateSave, &MainWindow::StateSave);
  handle(&MenuBar::StateLoadSlot, &MainWindow::StateLoadSlot);
  handle(&MenuBar::StateSaveSlot, &MainWindow::StateSaveSlot);
  handle(&MenuBar::StateLoadSlotAt, &MainWindow::StateLoadSlotAt);
  handle(&MenuBar::StateSaveSlotAt, &MainWindow::StateSaveSlotAt);
  handle(&MenuBar::StateLoadUndo, &MainWindow::StateLoadUndo);
  handle(&MenuBar::StateSaveUndo, &MainWindow::StateSaveUndo);
  handle(&MenuBar::StateSaveOldest, &MainWindow::StateSaveOldest);
  handle(&MenuBar::SetStateSlot, &MainWindow::SetStateSlot);

  // Options menu
  handle(&MenuBar::Configure, &MainWindow::ShowSettingsWindow);
  handle(&MenuBar::ConfigureGraphics, &MainWindow::ShowGraphicsWindow);
  handle(&MenuBar::ConfigureAudio, &MainWindow::ShowAudioWindow);
  handle(&MenuBar::ConfigureControllers, &MainWindow::ShowControllersWindow);
  handle(&MenuBar::ConfigureHotkeys, &MainWindow::ShowHotkeyDialog);
  handle(&MenuBar::ConfigureFreelook, &MainWindow::ShowFreeLookWindow);

  // Tools menu
  handle(&MenuBar::ShowMemcardManager, &MainWindow::ShowMemcardManager);
  handle(&MenuBar::ShowResourcePackManager, &MainWindow::ShowResourcePackManager);
  handle(&MenuBar::ShowCheatsManager, &MainWindow::ShowCheatsManager);
  handle(&MenuBar::BootGameCubeIPL, &MainWindow::OnBootGameCubeIPL);
  handle(&MenuBar::ImportNANDBackup, &MainWindow::OnImportNANDBackup);
  handle(&MenuBar::PerformOnlineUpdate, &MainWindow::PerformOnlineUpdate);
  handle(&MenuBar::BootWiiSystemMenu, &MainWindow::BootWiiSystemMenu);
  handle(&MenuBar::StartNetPlay, &MainWindow::ShowNetPlaySetupDialog);
  handle(&MenuBar::BrowseNetPlay, &MainWindow::ShowNetPlayBrowser);
  handle(&MenuBar::ShowFIFOPlayer, &MainWindow::ShowFIFOPlayer);
  handle(&MenuBar::ShowSkylanderPortal, &MainWindow::ShowSkylanderPortal);
  handle(&MenuBar::ShowInfinityBase, &MainWindow::ShowInfinityBase);
  handle(&MenuBar::ShowWiiSpeakWindow, &MainWindow::ShowWiiSpeakWindow);
  handle(&MenuBar::ShowLogitechMicWindow, &MainWindow::ShowLogitechMicWindow);
  handle(&MenuBar::ConnectWiiRemote, &MainWindow::OnConnectWiiRemote);
#ifdef USE_RETRO_ACHIEVEMENTS
  handle(&MenuBar::ShowAchievementsWindow, &MainWindow::ShowAchievementsWindow);
#endif  // USE_RETRO_ACHIEVEMENTS

  // Movie menu
  handle(&MenuBar::PlayRecording, &MainWindow::OnPlayRecording);
  handle(&MenuBar::StartRecording, &MainWindow::OnStartRecording);
  handle(&MenuBar::StopRecording, &MainWindow::OnStopRecording);
  handle(&MenuBar::ExportRecording, &MainWindow::OnExportRecording);
  handle(&MenuBar::ShowTASInput, &MainWindow::ShowTASInput);
  handle(&MenuBar::ConfigureOSD, &MainWindow::ShowOSDWindow);

  // View menu
  to_list(&MenuBar::ShowList, &GameList::SetListView);
  to_list(&MenuBar::ShowGrid, &GameList::SetGridView);
  to_list(&MenuBar::PurgeGameListCache, &GameList::PurgeCache);
  connect(m_menu_bar, &MenuBar::ShowSearch, m_search_bar, &SearchBar::Show);
  to_list(&MenuBar::ColumnVisibilityToggled, &GameList::OnColumnVisibilityToggled);
  to_list(&MenuBar::GameListPlatformVisibilityToggled, &GameList::OnGameListVisibilityChanged);
  to_list(&MenuBar::GameListRegionVisibilityToggled, &GameList::OnGameListVisibilityChanged);

  // Help menu
  handle(&MenuBar::ShowAboutDialog, &MainWindow::ShowAboutDialog);

  // State the menu bar mirrors
  connect(m_game_list, &GameList::SelectionChanged, m_menu_bar, &MenuBar::SelectionChanged);
  connect(this, &MainWindow::ReadOnlyModeChanged, m_menu_bar, &MenuBar::ReadOnlyModeChanged);
  connect(this, &MainWindow::RecordingStatusChanged, m_menu_bar, &MenuBar::RecordingStatusChanged);
}

void MainWindow::ConnectHotkeys()
{
  HotkeyScheduler* const hotkeys = m_hotkey_scheduler;
  const auto handle = [this, hotkeys](auto signal, auto slot) {
    connect(hotkeys, signal, this, slot);
  };
  const auto to_code = [this, hotkeys](auto signal, auto slot) {
    connect(hotkeys, signal, m_code_widget, slot);
  };

  handle(&HotkeyScheduler::Open, &MainWindow::Open);
  handle(&HotkeyScheduler::ChangeDisc, &MainWindow::ChangeDisc);
  handle(&HotkeyScheduler::EjectDisc, &MainWindow::EjectDisc);
  handle(&HotkeyScheduler::ExitHotkey, &MainWindow::close);
  handle(&HotkeyScheduler::UnlockCursor, &MainWindow::UnlockCursor);
  handle(&HotkeyScheduler::TogglePauseHotkey, &MainWindow::TogglePause);
  handle(&HotkeyScheduler::ActivateChat, &MainWindow::OnActivateChat);
  handle(&HotkeyScheduler::RequestGolfControl, &MainWindow::OnRequestGolfControl);
  handle(&HotkeyScheduler::RefreshGameListHotkey, &MainWindow::RefreshGameList);
  handle(&HotkeyScheduler::StopHotkey, &MainWindow::RequestStop);
  handle(&HotkeyScheduler::ResetHotkey, &MainWindow::Reset);
  handle(&HotkeyScheduler::ScreenShotHotkey, &MainWindow::ScreenShot);
  handle(&HotkeyScheduler::FullScreenHotkey, &MainWindow::FullScreen);

  // Save states
  handle(&HotkeyScheduler::StateLoadSlot, &MainWindow::StateLoadSlotAt);
  handle(&HotkeyScheduler::StateSaveSlot, &MainWindow::StateSaveSlotAt);
  handle(&HotkeyScheduler::StateLoadLastSaved, &MainWindow::StateLoadLastSavedAt);
  handle(&HotkeyScheduler::StateLoadUndo, &MainWindow::StateLoadUndo);
  handle(&HotkeyScheduler::StateSaveUndo, &MainWindow::StateSaveUndo);
  handle(&HotkeyScheduler::StateSaveOldest, &MainWindow::StateSaveOldest);
  handle(&HotkeyScheduler::StateSaveFile, &MainWindow::StateSave);
  handle(&HotkeyScheduler::StateLoadFile, &MainWindow::StateLoad);
  handle(&HotkeyScheduler::StateLoadSlotHotkey, &MainWindow::StateLoadSlot);
  handle(&HotkeyScheduler::StateSaveSlotHotkey, &MainWindow::StateSaveSlot);
  handle(&HotkeyScheduler::SetStateSlotHotkey, &MainWindow::SetStateSlot);
  handle(&HotkeyScheduler::IncrementSelectedStateSlotHotkey,
         &MainWindow::IncrementSelectedStateSlot);
  handle(&HotkeyScheduler::DecrementSelectedStateSlotHotkey,
         &MainWindow::DecrementSelectedStateSlot);

  // Movies and devices
  handle(&HotkeyScheduler::StartRecording, &MainWindow::OnStartRecording);
  handle(&HotkeyScheduler::PlayRecording, &MainWindow::OnPlayRecording);
  handle(&HotkeyScheduler::ExportRecording, &MainWindow::OnExportRecording);
  handle(&HotkeyScheduler::ConnectWiiRemote, &MainWindow::OnConnectWiiRemote);
  handle(&HotkeyScheduler::ToggleReadOnlyMode, &MainWindow::ToggleReadOnlyMode);
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(hotkeys, &HotkeyScheduler::OpenAchievements, this, &MainWindow::ShowAchievementsWindow,
          Qt::QueuedConnection);
#endif  // USE_RETRO_ACHIEVEMENTS

  // Debugger
  to_code(&HotkeyScheduler::Step, &CodeWidget::Step);
  to_code(&HotkeyScheduler::StepOver, &CodeWidget::StepOver);
  to_code(&HotkeyScheduler::StepOut, &CodeWidget::StepOut);
  to_code(&HotkeyScheduler::Skip, &CodeWidget::Skip);
  to_code(&HotkeyScheduler::ShowPC, &CodeWidget::ShowPC);
  to_code(&HotkeyScheduler::SetPC, &CodeWidget::SetPC);
  to_code(&HotkeyScheduler::ToggleBreakpoint, &CodeWidget::ToggleBreakpoint);
  to_code(&HotkeyScheduler::AddBreakpoint, &CodeWidget::AddBreakpoint);

  // Emulated USB toys
  handle(&HotkeyScheduler::SkylandersPortalHotkey, &MainWindow::ShowSkylanderPortal);
  handle(&HotkeyScheduler::InfinityBaseHotkey, &MainWindow::ShowInfinityBase);
}

void MainWindow::ConnectToolBar()
{
  addToolBar(m_tool_bar);

  const auto handle = [this](auto signal, auto slot) { connect(m_tool_bar, signal, this, slot); };
  const auto to_code = [this](auto signal, auto slot) {
    connect(m_tool_bar, signal, m_code_widget, slot);
  };

  handle(&ToolBar::OpenPressed, &MainWindow::Open);
  handle(&ToolBar::RefreshPressed, &MainWindow::RefreshGameList);
  handle(&ToolBar::PlayPressed, [this] { Play(); });
  handle(&ToolBar::PausePressed, &MainWindow::Pause);
  handle(&ToolBar::StopPressed, &MainWindow::RequestStop);
  handle(&ToolBar::FullScreenPressed, &MainWindow::FullScreen);
  handle(&ToolBar::ScreenShotPressed, &MainWindow::ScreenShot);
  handle(&ToolBar::SettingsPressed, &MainWindow::ShowSettingsWindow);
  handle(&ToolBar::ControllersPressed, &MainWindow::ShowControllersWindow);
  handle(&ToolBar::GraphicsPressed, &MainWindow::ShowGraphicsWindow);

  to_code(&ToolBar::StepPressed, &CodeWidget::Step);
  to_code(&ToolBar::StepOverPressed, &CodeWidget::StepOver);
  to_code(&ToolBar::StepOutPressed, &CodeWidget::StepOut);
  to_code(&ToolBar::SkipPressed, &CodeWidget::Skip);
  to_code(&ToolBar::ShowPCPressed, &CodeWidget::ShowPC);
  to_code(&ToolBar::SetPCPressed, &CodeWidget::SetPC);
}

void MainWindow::ConnectGameList()
{
  connect(m_game_list, &GameList::GameSelected, this, [this] { Play(); });
  connect(m_game_list, &GameList::NetPlayHost, this, &MainWindow::NetPlayHost);
  connect(m_game_list, &GameList::OnStartWithRiivolution, this,
          &MainWindow::ShowRiivolutionBootWidget);
  connect(m_game_list, &GameList::OpenGeneralSettings, this, &MainWindow::ShowGeneralWindow);
  connect(m_game_list, &GameList::OpenGraphicsSettings, this, &MainWindow::ShowGraphicsWindow);
}

void MainWindow::ConnectRenderWidget()
{
  m_rendering_to_main = false;
  m_render_widget->hide();
  WatchRenderWidget();
}

// Connections every render widget gets, including the replacements HideRenderWidget creates.
void MainWindow::WatchRenderWidget()
{
  connect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);
  connect(m_render_widget, &RenderWidget::FocusChanged, this, [this](bool has_focus) {
    // An exclusive fullscreen mode is only kept while the game window has focus.
    if (m_render_widget->isFullScreen())
      SetFullScreenResolution(has_focus);
  });
}

void MainWindow::ConnectHost()
{
  connect(Host::GetInstance(), &Host::RequestStop, this, &MainWindow::RequestStop);
}

void MainWindow::ConnectStack()
{
  // Page 0 of the stack: the game list with the search bar and the game count below it.
  auto* const list_page = new QWidget;
  auto* const column = new QVBoxLayout;
  list_page->setLayout(column);
  for (QWidget* part : {static_cast<QWidget*>(m_game_list), static_cast<QWidget*>(m_search_bar),
                        static_cast<QWidget*>(m_game_count)})
  {
    column->addWidget(part);
  }
  column->setSpacing(0);
  column->setContentsMargins(0, 0, 0, 0);

  connect(m_search_bar, &SearchBar::Search, m_game_list, &GameList::SetSearchTerm);
  connect(m_game_list, &GameList::GameCountUpdated, m_game_count, &GameCount::OnGameCountUpdated);
  m_game_list->UpdateGameCount();

  // The search bar's bottom margin and the game count's top margin each look right alone, but
  // stacked together they leave too wide a gap. While the count is shown the search bar drops
  // its bottom margin; otherwise the bottom margin matches the top one.
  const auto show_game_count = [this](bool visible) {
    m_game_count->setVisible(visible);
    QLayout* const search_layout = m_search_bar->layout();
    QMargins margins = search_layout->contentsMargins();
    margins.setBottom(visible ? 0 : margins.top());
    search_layout->setContentsMargins(margins);
  };
  show_game_count(Settings::Instance().IsGameCountVisible());
  connect(&Settings::Instance(), &Settings::GameCountVisibilityChanged, this, show_game_count);

  m_stack->addWidget(list_page);
  setCentralWidget(m_stack);

  // Debugger docks: all on the left, tabbed together behind the log.
  setDockOptions(DockOption::AllowNestedDocks | DockOption::AllowTabbedDocks);
  setTabPosition(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea, QTabWidget::North);

  QDockWidget* const docks[] = {m_log_widget,      m_log_config_widget, m_code_widget,
                                m_register_widget, m_thread_widget,     m_watch_widget,
                                m_breakpoint_widget, m_memory_widget,   m_network_widget,
                                m_jit_widget,      m_assembler_widget};
  for (QDockWidget* dock : docks)
    addDockWidget(Qt::LeftDockWidgetArea, dock);
  for (QDockWidget* dock : docks)
  {
    if (dock != m_log_widget)
      tabifyDockWidget(m_log_widget, dock);
  }
}

void MainWindow::RefreshGameList()
{
  Settings& settings = Settings::Instance();
  settings.ReloadTitleDB();
  settings.RefreshGameList();
}

QStringList MainWindow::PromptFileNames()
{
  QSettings& qsettings = Settings::Instance().GetQSettings();
  const QString last_directory =
      qsettings.value(QStringLiteral("mainwindow/lastdir"), QString{}).toString();
  const QString filter =
      QStringLiteral("%1 (*.elf *.dol *.gcm *.bin *.iso *.tgc *.wbfs *.ciso *.gcz *.wia *.rvz "
                     "hif_000000.nfs *.wad *.dff *.m3u *.json);;%2 (*)")
          .arg(tr("All GC/Wii files"))
          .arg(tr("All Files"));

  const QStringList chosen =
      DolphinFileDialog::getOpenFileNames(this, tr("Select a File"), last_directory, filter);
  if (!chosen.isEmpty())
  {
    qsettings.setValue(QStringLiteral("mainwindow/lastdir"),
                       QFileInfo(chosen.front()).absoluteDir().absolutePath());
  }
  return chosen;
}

void MainWindow::ChangeDisc()
{
  const std::vector<std::string> discs = ToStdStrings(PromptFileNames());
  if (discs.empty())
    return;

  const Core::CPUThreadGuard guard(m_system);
  m_system.GetDVDInterface().ChangeDisc(guard, discs);
}

void MainWindow::EjectDisc()
{
  const Core::CPUThreadGuard guard(m_system);
  m_system.GetDVDInterface().EjectDisc(guard, DVD::EjectCause::User);
}

void MainWindow::OpenUserFolder()
{
  OpenUserDirectory(D_USER_IDX);
}

void MainWindow::OpenConfigFolder()
{
  OpenUserDirectory(D_CONFIG_IDX);
}

void MainWindow::OpenCacheFolder()
{
  OpenUserDirectory(D_CACHE_IDX);
}

void MainWindow::Open()
{
  const QStringList chosen = PromptFileNames();
  if (chosen.isEmpty())
    return;
  StartGame(ToStdStrings(chosen));
}

// Resumes a paused game; otherwise boots, in order of preference, the game selected in the
// list or the configured default ISO, and failing both asks for a file.
void MainWindow::Play(const std::optional<std::string>& savestate_path)
{
  if (Core::GetState(m_system) == Core::State::Paused)
  {
    Core::SetState(m_system, Core::State::Running);
    return;
  }

  if (const auto selected = m_game_list->GetSelectedGame())
  {
    StartGame(selected->GetFilePath(), ScanForSecondDisc::Yes, SessionWithState(savestate_path));
    return;
  }

  const QString default_iso = QString::fromStdString(Config::Get(Config::MAIN_DEFAULT_ISO));
  if (default_iso.isEmpty() || !QFile::exists(default_iso))
  {
    Open();
    return;
  }
  StartGame(default_iso, ScanForSecondDisc::Yes, SessionWithState(savestate_path));
}

void MainWindow::Pause()
{
  Core::SetState(m_system, Core::State::Paused);
}

void MainWindow::TogglePause()
{
  if (Core::GetState(m_system) != Core::State::Paused)
    Pause();
  else
    Play();
}

void MainWindow::OnStopComplete()
{
  m_stop_requested = false;
  HideRenderWidget(!m_exit_requested, m_exit_requested);
#ifdef USE_DISCORD_PRESENCE
  if (!m_netplay_dialog->isVisible())
    Discord::UpdateDiscordPresence();
#endif

  SetFullScreenResolution(false);

  const bool quitting = m_exit_requested || Settings::Instance().IsBatchModeEnabled();
  if (quitting)
  {
    // The assembler may still hold unsaved files and gets a chance to cancel.
    if (m_assembler_widget->ApplicationCloseRequest())
      QGuiApplication::exit(0);
    else
      m_exit_requested = false;
  }

  // A boot that had to wait for this emulation to end can go ahead now.
  if (m_pending_boot != nullptr)
  {
    StartGame(std::move(m_pending_boot));
    m_pending_boot.reset();
  }
}

bool MainWindow::RequestStop()
{
  if (Core::IsUninitialized(m_system))
  {
    // Nothing is running; still finish "stopping" so that pending exits and boots go ahead.
    Core::QueueHostJob([this](Core::System&) { OnStopComplete(); }, true);
    return true;
  }

  const bool render_widget_was_active =
      Settings::Instance().IsKeepWindowOnTopEnabled() ||
      (m_render_widget->isActiveWindow() && !m_render_widget->isFullScreen());
  QWidget* const question_parent =
      (render_widget_was_active && !m_rendering_to_main) ? static_cast<QWidget*>(m_render_widget) :
                                                           static_cast<QWidget*>(this);
  const bool was_cursor_locked = m_render_widget->IsCursorLocked();

  if (m_render_widget->isFullScreen())
  {
    FullScreen();
  }
  else if (!RenderWidget::IsEmbedMode())
  {
    // PixelU: in embed mode the window is parked off-screen or docked inside RetroPixels, so its
    // geometry would overwrite the user's normal window placement with nonsense.
    m_render_widget_geometry = m_render_widget->saveGeometry();
  }

  bool ask_first = Config::Get(Config::MAIN_CONFIRM_ON_STOP);
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  ask_first = ask_first || AchievementManager::GetInstance().CheckForModifications();
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
  if (ask_first)
  {
    switch (AskBeforeStopping(question_parent, render_widget_was_active, was_cursor_locked))
    {
    case StopAnswer::AlreadyAsking:
      return true;
    case StopAnswer::KeepPlaying:
      return false;
    case StopAnswer::Stop:
      break;
    }
  }

  OnStopRecording();

  // The first request asks the game to shut down by itself (a power button press) where the
  // console supports it; asking again, or a console without that, stops emulation outright.
  if (!m_stop_requested && UICommon::TriggerSTMPowerEvent())
  {
    m_stop_requested = true;

    // The game has to run to notice the power button.
    if (Core::GetState(m_system) == Core::State::Paused)
      Core::SetState(m_system, Core::State::Running);

    if (NetPlay::IsNetPlayRunning())
      NetPlay::SendPowerButtonEvent();
    return true;
  }

  ForceStop();
#ifdef Q_OS_WIN
  // Let Windows idle and blank the display again.
  SetThreadExecutionState(ES_CONTINUOUS);
#endif
  return true;
}

MainWindow::StopAnswer MainWindow::AskBeforeStopping(QWidget* parent,
                                                     bool render_widget_was_active,
                                                     bool was_cursor_locked)
{
  if (m_stop_confirm_showing)
    return StopAnswer::AlreadyAsking;
  m_stop_confirm_showing = true;
  Common::ScopeGuard question_done([this] { m_stop_confirm_showing = false; });

  // The game is paused while the question is up, except under NetPlay where that would stall
  // everyone else.
  const Core::State state_before = Core::GetState(m_system);
  const bool pause_while_asking = !Settings::Instance().GetNetPlayClient();
  if (pause_while_asking)
    Core::SetState(m_system, Core::State::Paused);

  // Both of these must happen before the box exists: its own activation and deactivation
  // events would otherwise reach the render widget before it knows what to do with the cursor.
  if (render_widget_was_active)
    m_render_widget->SetCursorLockedOnNextActivation(was_cursor_locked);
  m_render_widget->SetWaitingForMessageBox(true);

  QString question = tr("Do you want to stop the current emulation?");
  if (m_stop_requested)
  {
    question = tr("A shutdown is already in progress. Unsaved data "
                  "may be lost if you stop the current emulation "
                  "before it completes. Force stop?");
  }
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  else if (AchievementManager::GetInstance().CheckForModifications())
  {
    question = tr(
        "Do you want to stop the current emulation? Unsaved achievement modifications detected.");
  }
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

  const auto answer =
      ModalMessageBox::question(parent, tr("Confirm"), question, QMessageBox::Yes | QMessageBox::No,
                                QMessageBox::NoButton, Qt::ApplicationModal);

  if (answer == QMessageBox::Yes)
  {
    // Stopping: the cursor stays free even though the render widget lives on for a moment.
    // The order matters, SetWaitingForMessageBox looks at the lock request.
    m_render_widget->SetCursorLockedOnNextActivation(false);
    m_render_widget->SetWaitingForMessageBox(false);
    return StopAnswer::Stop;
  }

  // Carrying on: put everything back as it was, cursor lock included.
  m_render_widget->SetWaitingForMessageBox(false);
  if (pause_while_asking)
    Core::SetState(m_system, state_before);
  return StopAnswer::KeepPlaying;
}

void MainWindow::ForceStop()
{
  Core::Stop(m_system);
}

void MainWindow::Reset()
{
  Movie::MovieManager& movie = m_system.GetMovie();
  if (movie.IsRecordingInput())
    movie.SetReset(true);
  m_system.GetProcessorInterface().ResetButton_Tap();
}

void MainWindow::FrameAdvance()
{
  Core::DoFrameStep(m_system);
}

void MainWindow::FullScreen()
{
  // Leaving fullscreen puts the window back the way the settings say; entering it remembers the
  // windowed geometry first.
  const bool leaving_fullscreen = m_render_widget->isFullScreen();
  if (!leaving_fullscreen)
    m_render_widget_geometry = m_render_widget->saveGeometry();

  HideRenderWidget(false);
  SetFullScreenResolution(!leaving_fullscreen);

  if (leaving_fullscreen)
    ShowRenderWidget();
  else
    m_render_widget->showFullScreen();
}

void MainWindow::UnlockCursor()
{
  if (m_render_widget->isFullScreen())
    return;
  m_render_widget->SetCursorLocked(false);
}

void MainWindow::ScreenShot()
{
  Core::SaveScreenShot();
}

// The game's own file followed by its second disc, when the game list knows of one.
std::vector<std::string> MainWindow::DiscSetPaths(const UICommon::GameFile& game) const
{
  const auto second_disc = m_game_list->FindSecondDisc(game);
  std::vector<std::string> paths{game.GetFilePath()};
  if (second_disc != nullptr)
    paths.push_back(second_disc->GetFilePath());
  return paths;
}

void MainWindow::ScanForSecondDiscAndStartGame(const UICommon::GameFile& game,
                                               std::unique_ptr<BootSessionData> boot_session_data)
{
  StartGame(DiscSetPaths(game), std::move(boot_session_data));
}

void MainWindow::StartGame(const QString& path, ScanForSecondDisc scan,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  StartGame(path.toStdString(), scan, std::move(boot_session_data));
}

void MainWindow::StartGame(const std::string& path, ScanForSecondDisc scan,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  if (scan == ScanForSecondDisc::Yes)
  {
    if (const auto known_game = m_game_list->FindGame(path))
    {
      ScanForSecondDiscAndStartGame(*known_game, std::move(boot_session_data));
      return;
    }
  }

  BootSessionData session = boot_session_data ? std::move(*boot_session_data) : BootSessionData();
  StartGame(BootParameters::GenerateFromFile(path, std::move(session)));
}

void MainWindow::StartGame(const std::vector<std::string>& paths,
                           std::unique_ptr<BootSessionData> boot_session_data)
{
  BootSessionData session = boot_session_data ? std::move(*boot_session_data) : BootSessionData();
  StartGame(BootParameters::GenerateFromFile(paths, std::move(session)));
}

// Warnings for discs about to boot. Returns false when the user turned down an NKit image.
bool MainWindow::ConfirmDiscCanBoot(const BootParameters& parameters)
{
  const auto* const disc = std::get_if<BootParameters::Disc>(&parameters.parameters);
  if (disc == nullptr)
    return true;

  if (disc->volume->IsNKit() && !NKitWarningDialog::ShowUnlessDisabled())
    return false;

  if (disc->volume->GetVolumeType() == DiscIO::Platform::Triforce)
    return true;

  // A Triforce baseboard in either place stops ordinary games from booting. (Some Triforce
  // tools have no boot.id and are not recognised as Triforce discs, yet still boot.)
  const bool baseboard_in_sp1 =
      Config::Get(Config::MAIN_SERIAL_PORT_1) == ExpansionInterface::EXIDeviceType::Baseboard;
  const bool baseboard_in_port_1 = Config::Get(Config::GetInfoForSIDevice(0)) ==
                                   SerialInterface::SIDevices::SIDEVICE_AM_BASEBOARD;
  if (baseboard_in_sp1)
  {
    ModalMessageBox::warning(this, tr("Warning"),
                             tr("Non-Triforce games cannot be booted with Triforce hardware "
                                "attached.\nPlease remove the Triforce Baseboard from SP1."),
                             QMessageBox::Ok);
  }
  if (baseboard_in_port_1)
  {
    ModalMessageBox::warning(this, tr("Warning"),
                             tr("Non-Triforce games cannot be booted with Triforce hardware "
                                "attached.\nPlease remove the Triforce Baseboard from Port 1."),
                             QMessageBox::Ok);
  }
  return true;
}

void MainWindow::StartGame(std::unique_ptr<BootParameters>&& parameters)
{
  if (parameters != nullptr && !ConfirmDiscCanBoot(*parameters))
    return;

  if (!Core::IsUninitialized(m_system))
  {
    // Something is still running. Stop it first and boot once it is gone (OnStopComplete),
    // unless the user would rather keep playing.
    if (RequestStop())
      m_pending_boot = std::move(parameters);
    return;
  }

  // The core needs the render widget's native window to boot into.
  ShowRenderWidget();

  if (!BootManager::BootCore(m_system, std::move(parameters),
                             DescribeWindow(m_render_widget->windowHandle())))
  {
    ModalMessageBox::critical(this, tr("Error"), tr("Failed to init core"), QMessageBox::Ok);
    HideRenderWidget();
    return;
  }

#ifdef USE_DISCORD_PRESENCE
  if (!NetPlay::IsNetPlayRunning())
    Discord::UpdateDiscordPresence();
#endif

  // Fullscreen is entered once the core reports it is running (see InitCoreCallbacks).
  if (Config::Get(Config::MAIN_FULLSCREEN))
    m_fullscreen_requested = true;
}

// Switches the monitor to the configured fullscreen display mode, or back to the desktop's.
// "Auto" leaves the display mode alone.
void MainWindow::SetFullScreenResolution(bool fullscreen)
{
  const std::string resolution = Config::Get(Config::MAIN_FULLSCREEN_DISPLAY_RES);
  if (resolution == "Auto")
    return;

#ifdef _WIN32
  if (!fullscreen)
  {
    ChangeDisplaySettings(nullptr, CDS_FULLSCREEN);
    return;
  }

  // "WIDTHxHEIGHT"; a part that does not parse stays 0.
  DEVMODE mode{};
  mode.dmSize = sizeof(mode);
  std::sscanf(resolution.c_str(), "%lux%lu", &mode.dmPelsWidth, &mode.dmPelsHeight);
  mode.dmBitsPerPel = 32;
  mode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
  // CDS_FULLSCREEN makes the change temporary and keeps the taskbar out of the way.
  ChangeDisplaySettings(&mode, CDS_FULLSCREEN);
#elif defined(HAVE_XRANDR) && HAVE_XRANDR
  if (m_xrr_config != nullptr)
    m_xrr_config->ToggleDisplayMode(fullscreen);
#else
  static_cast<void>(fullscreen);
#endif
}

void MainWindow::ShowRenderWidget()
{
  SetFullScreenResolution(false);
  Host::GetInstance()->SetRenderFullscreen(false);

  m_rendering_to_main = Config::Get(Config::MAIN_RENDER_TO_MAIN);
  if (m_rendering_to_main)
  {
    // The game takes the place of the game list, and its title becomes the window's.
    m_stack->setCurrentIndex(m_stack->addWidget(m_render_widget));
    connect(Host::GetInstance(), &Host::RequestTitle, this, &MainWindow::setWindowTitle);
    m_stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_stack->repaint();

    Host::GetInstance()->SetRenderFocus(isActiveWindow());
    return;
  }

  if (RenderWidget::IsEmbedMode())
  {
    // PixelU: RetroPixels docks the render window into its own; until then it stays out of
    // sight, off-screen and without a taskbar button.
    m_render_widget->ShowForEmbedding(m_render_widget_geometry);
    return;
  }

  m_render_widget->showNormal();
  m_render_widget->restoreGeometry(m_render_widget_geometry);
}

void MainWindow::HideRenderWidget(bool reinit, bool is_exit)
{
  if (m_rendering_to_main)
  {
    // Take the game out of the main window; parentless, it could become a window of its own.
    m_stack->removeWidget(m_render_widget);
    m_render_widget->setParent(nullptr);
    m_rendering_to_main = false;
    m_stack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    disconnect(Host::GetInstance(), &Host::RequestTitle, this, &MainWindow::setWindowTitle);
    setWindowTitle(IdleWindowTitle());
  }

  if (!reinit)
    return;

  // Some drivers crash when a different graphics backend reuses the native window of the last
  // one, so every session gets a brand-new render widget.
  m_render_widget->hide();
  disconnect(m_render_widget, &RenderWidget::Closed, this, &MainWindow::ForceStop);
  m_render_widget->removeEventFilter(this);
  m_render_widget->deleteLater();

  m_render_widget = new RenderWidget;
  m_render_widget->installEventFilter(this);
  WatchRenderWidget();

  // Input may still be bound to the old widget's window. Point it at the main window until the
  // next boot moves it to the new render widget.
  const auto reason = is_exit ? ControllerInterface::WindowChangeReason::Exit :
                                ControllerInterface::WindowChangeReason::Other;
  g_controller_interface.ChangeWindow(DescribeWindow(windowHandle()).render_window, reason);
}

void MainWindow::ShowSettingsWindow()
{
  Summon(m_settings_window, [this] {
#ifdef HAVE_XRANDR
    if (DetectWindowSystem() == WindowSystemType::X11)
    {
      auto* const display = static_cast<Display*>(
          QGuiApplication::platformNativeInterface()->nativeResourceForWindow("display",
                                                                              windowHandle()));
      m_xrr_config = std::make_unique<X11Utils::XRRConfiguration>(display, winId());
    }
#endif
    auto* const window = new SettingsWindow(this);
    SuspendHotkeysWhileActive(window);
    return window;
  });
}

void MainWindow::ShowSettingsPane(SettingsWindowPaneIndex pane)
{
  ShowSettingsWindow();
  m_settings_window->SelectPane(pane);
}

void MainWindow::ShowControllersWindow()
{
  ShowSettingsPane(SettingsWindowPaneIndex::Controllers);
}

void MainWindow::ShowTriforceWindow()
{
  ShowSettingsPane(SettingsWindowPaneIndex::Triforce);
}

void MainWindow::ShowAudioWindow()
{
  ShowSettingsPane(SettingsWindowPaneIndex::Audio);
}

void MainWindow::ShowGeneralWindow()
{
  ShowSettingsPane(SettingsWindowPaneIndex::General);
}

void MainWindow::ShowOSDWindow()
{
  ShowSettingsPane(SettingsWindowPaneIndex::OnScreenDisplay);
}

void MainWindow::ShowGraphicsWindow()
{
  ShowSettingsPane(SettingsWindowPaneIndex::Graphics);
}

void MainWindow::ShowFreeLookWindow()
{
  Summon(m_freelook_window, [this] {
    auto* const window = new FreeLookWindow(this);
    SuspendHotkeysWhileActive(window);
#ifdef USE_RETRO_ACHIEVEMENTS
    connect(window, &FreeLookWindow::OpenAchievementSettings, this,
            &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
    return window;
  });
}

void MainWindow::ShowAboutDialog()
{
  AboutDialog(this).exec();
}

void MainWindow::ShowHotkeyDialog()
{
  Summon(m_hotkey_window, [this] {
    auto* const window = new MappingWindow(this, MappingWindow::Type::MAPPING_HOTKEYS, 0);
    SuspendHotkeysWhileActive(window);
    return window;
  });
}

void MainWindow::ShowNetPlaySetupDialog()
{
  BringToFront(m_netplay_setup_dialog);
}

void MainWindow::ShowNetPlayBrowser()
{
  auto* const browser = new NetPlayBrowser(this);
  browser->setAttribute(Qt::WA_DeleteOnClose, true);
  connect(browser, &NetPlayBrowser::Join, this, &MainWindow::NetPlayJoin);
  browser->exec();
}

void MainWindow::ShowFIFOPlayer()
{
  if (m_fifo_window == nullptr)
  {
    m_fifo_window =
        std::make_unique<FIFOPlayerWindow>(m_system.GetFifoPlayer(), m_system.GetFifoRecorder());
    connect(m_fifo_window.get(), &FIFOPlayerWindow::LoadFIFORequested, this,
            [this](const QString& file) { StartGame(file, ScanForSecondDisc::No); });
  }
  BringToFront(m_fifo_window.get());
}

void MainWindow::ShowSkylanderPortal()
{
  Summon(m_skylander_window, [] { return new SkylanderPortalWindow(); });
}

void MainWindow::ShowInfinityBase()
{
  Summon(m_infinity_window, [] { return new InfinityBaseWindow(); });
}

void MainWindow::ShowWiiSpeakWindow()
{
  Summon(m_wii_speak_window, [] { return new WiiSpeakWindow(); });
}

void MainWindow::ShowLogitechMicWindow()
{
  Summon(m_logitech_mic_window, [] { return new LogitechMicWindow(); });
}

// Where the save state file dialogs open: the last folder used, or the working directory.
QString MainWindow::StateFileDialogDirectory() const
{
  const std::string last_folder = Config::Get(Config::MAIN_CURRENT_STATE_PATH);
  if (last_folder.empty())
    return QDir::currentPath();
  return QString::fromStdString(last_folder);
}

void MainWindow::StateLoad()
{
  const QString file =
      DolphinFileDialog::getOpenFileName(this, tr("Select a File"), StateFileDialogDirectory(),
                                         tr("All Save States (*.sav *.s??);; All Files (*)"));
  // The folder is remembered even when the dialog was cancelled.
  Config::SetBase(Config::MAIN_CURRENT_STATE_PATH, QFileInfo(file).dir().path().toStdString());
  if (file.isEmpty())
    return;
  State::LoadAs(m_system, file.toStdString());
}

void MainWindow::StateSave()
{
  const QString file =
      DolphinFileDialog::getSaveFileName(this, tr("Select a File"), StateFileDialogDirectory(),
                                         tr("All Save States (*.sav *.s??);; All Files (*)"));
  Config::SetBase(Config::MAIN_CURRENT_STATE_PATH, QFileInfo(file).dir().path().toStdString());
  if (file.isEmpty())
    return;
  State::SaveAs(m_system, file.toStdString());
}

void MainWindow::StateLoadSlot()
{
  State::Load(m_system, m_state_slot);
}

void MainWindow::StateSaveSlot()
{
  State::Save(m_system, m_state_slot);
}

void MainWindow::StateLoadSlotAt(int slot)
{
  State::Load(m_system, static_cast<u32>(slot));
}

void MainWindow::StateLoadLastSavedAt(int slot)
{
  State::LoadLastSaved(m_system, slot);
}

void MainWindow::StateSaveSlotAt(int slot)
{
  State::Save(m_system, static_cast<u32>(slot));
}

void MainWindow::StateSaveOldest()
{
  State::SaveFirstSaved(m_system);
}

void MainWindow::StateSaveUndo()
{
  State::UndoSaveState(m_system);
}

void MainWindow::StateLoadUndo()
{
  State::UndoLoadState(m_system);
}

void MainWindow::SetStateSlot(int slot)
{
  Settings::Instance().SetStateSlot(slot);
  m_state_slot = static_cast<u32>(slot);

  const std::string slot_info = State::GetInfoStringOfSlot(m_state_slot, false);
  Core::DisplayMessage(fmt::format("Selected slot {} - {}", m_state_slot, slot_info), 2500);
}

// The slot hotkeys go through the menu bar so that its slot menu stays in step; it calls
// SetStateSlot in turn. Both wrap around at the ends.
void MainWindow::IncrementSelectedStateSlot()
{
  const u32 next = m_state_slot + 1;
  m_menu_bar->SetStateSlot(static_cast<int>(next > State::NUM_STATES ? 1 : next));
}

void MainWindow::DecrementSelectedStateSlot()
{
  const u32 previous = m_state_slot - 1;
  m_menu_bar->SetStateSlot(static_cast<int>(previous == 0 ? State::NUM_STATES : previous));
}

void MainWindow::PerformOnlineUpdate(const std::string& region)
{
  WiiUpdate::PerformOnlineUpdate(region, this);
  // The update may have brought a newer System Menu.
  Settings::Instance().NANDRefresh();
}

void MainWindow::BootWiiSystemMenu()
{
  StartGame(std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU}));
}

void MainWindow::NetPlayInit()
{
  const auto& games = m_game_list->GetGameListModel();

  // The setup dialog picks between joining and hosting.
  m_netplay_setup_dialog = new NetPlaySetupDialog(games, this);
  connect(m_netplay_setup_dialog, &NetPlaySetupDialog::Join, this, &MainWindow::NetPlayJoin);
  connect(m_netplay_setup_dialog, &NetPlaySetupDialog::Host, this, &MainWindow::NetPlayHost);

  // The session dialog has no parent; the destructor deletes it.
  const auto boot_for_session = [this](const std::string& path,
                                       std::unique_ptr<BootSessionData> session) {
    StartGame(path, ScanForSecondDisc::Yes, std::move(session));
  };
  m_netplay_dialog = new NetPlayDialog(games, boot_for_session);
  connect(m_netplay_dialog, &NetPlayDialog::Stop, this, &MainWindow::ForceStop);
  connect(m_netplay_dialog, &NetPlayDialog::rejected, this, &MainWindow::NetPlayQuit);

#ifdef USE_DISCORD_PRESENCE
  // Joining through a Discord invitation.
  m_netplay_discord = new DiscordHandler(this);
  connect(m_netplay_discord, &DiscordHandler::Join, this, &MainWindow::NetPlayJoin);
  Discord::InitNetPlayFunctionality(*m_netplay_discord);
  m_netplay_discord->Start();
#endif

  connect(&Settings::Instance(), &Settings::ConfigChanged, this,
          &MainWindow::UpdateScreenSaverInhibition);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &MainWindow::UpdateScreenSaverInhibition);
}

// A NetPlay session can only begin with no game running and no other session open.
bool MainWindow::NetPlayCanStart()
{
  QString problem;
  if (!Core::IsUninitialized(m_system))
    problem = tr("Can't start a NetPlay Session while a game is still running!");
  else if (m_netplay_dialog->isVisible())
    problem = tr("A NetPlay Session is already in progress!");
  else
    return true;

  ModalMessageBox::critical(nullptr, tr("Error"), problem);
  return false;
}

bool MainWindow::NetPlayJoin()
{
  if (!NetPlayCanStart())
    return false;

  const auto local_server = Settings::Instance().GetNetPlayServer();
  const bool hosting = local_server != nullptr;
  const bool use_traversal = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE) == "traversal";

  // Our own server is joined over loopback; otherwise use the configured address or host code.
  std::string address = "127.0.0.1";
  u16 port = 0;
  if (hosting)
  {
    port = local_server->GetPort();
  }
  else
  {
    address = use_traversal ? Config::Get(Config::NETPLAY_HOST_CODE) :
                              Config::Get(Config::NETPLAY_ADDRESS);
    port = Config::Get(Config::NETPLAY_CONNECT_PORT);
  }

  const std::string traversal_server = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const std::string nickname = Config::Get(Config::NETPLAY_NICKNAME);
  const std::string mode = Config::Get(Config::NETPLAY_NETWORK_MODE);

  if (hosting)
  {
    local_server->SetHostInputAuthority(mode == "hostinputauthority" || mode == "golf");
    local_server->AdjustPadBufferSize(Config::Get(Config::NETPLAY_BUFFER_SIZE));
  }

  // The host reaches its own server directly, never through the traversal server.
  const NetPlay::NetTraversalConfig traversal{!hosting && use_traversal, traversal_server,
                                              traversal_port};
  Settings::Instance().ResetNetPlayClient(
      new NetPlay::NetPlayClient(address, port, m_netplay_dialog, nickname, traversal));

  if (!Settings::Instance().GetNetPlayClient()->IsConnected())
  {
    NetPlayQuit();
    return false;
  }

  m_netplay_setup_dialog->close();
  m_netplay_dialog->show(nickname, use_traversal);
  return true;
}

bool MainWindow::NetPlayHost(const UICommon::GameFile& game)
{
  if (!NetPlayCanStart())
    return false;

  const bool use_traversal = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE) == "traversal";
  const bool use_upnp = Config::Get(Config::NETPLAY_USE_UPNP);
  const std::string traversal_server = Config::Get(Config::NETPLAY_TRAVERSAL_SERVER);
  const u16 traversal_port = Config::Get(Config::NETPLAY_TRAVERSAL_PORT);
  const u16 traversal_port_alt = Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT);
  const u16 port = Config::Get(Config::NETPLAY_HOST_PORT);
  const u16 listen_port = use_traversal ? Config::Get(Config::NETPLAY_LISTEN_PORT) : port;

  const NetPlay::NetTraversalConfig traversal{use_traversal, traversal_server, traversal_port,
                                              traversal_port_alt};
  Settings::Instance().ResetNetPlayServer(
      new NetPlay::NetPlayServer(listen_port, use_upnp, m_netplay_dialog, traversal));

  if (!Settings::Instance().GetNetPlayServer()->is_connected)
  {
    ModalMessageBox::critical(
        nullptr, tr("Failed to open server"),
        tr("Failed to listen on port %1. Is another instance of the NetPlay server running?")
            .arg(listen_port));
    NetPlayQuit();
    return false;
  }

  Settings::Instance().GetNetPlayServer()->ChangeGame(game.GetSyncIdentifier(),
                                                      m_game_list->GetNetPlayName(game));

  // The host plays too, as a client of its own server.
  return NetPlayJoin();
}

void MainWindow::NetPlayQuit()
{
  Settings::Instance().ResetNetPlayClient();
  Settings::Instance().ResetNetPlayServer();
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
}

void MainWindow::UpdateScreenSaverInhibition()
{
  const bool should_inhibit = Config::Get(Config::MAIN_DISABLE_SCREENSAVER) &&
                              Core::GetState(m_system) == Core::State::Running;
  if (should_inhibit != m_is_screensaver_inhibited)
  {
    m_is_screensaver_inhibited = should_inhibit;
    UICommon::InhibitScreenSaver(should_inhibit);
#if defined(QT_DBUS_LIB)
    InhibitDesktopScreenSaver(should_inhibit);
#endif
  }
}

// Closing the main window or the render widget is a stop request; the application quits once
// the stop completes, and only if it was the main window being closed.
bool MainWindow::eventFilter(QObject* object, QEvent* event)
{
  if (event->type() != QEvent::Close)
    return false;

  if (RequestStop() && object == this)
    m_exit_requested = true;

  static_cast<QCloseEvent*>(event)->ignore();
  return true;
}

QMenu* MainWindow::createPopupMenu()
{
  // Qt's default context menu lists every dock and toolbar, debugger docks included, even
  // while the debugger is hidden; that only confused people, so there is none.
  return nullptr;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  const QMimeData* const mime = event->mimeData();
  if (mime->hasUrls() && mime->urls().size() == 1)
    event->acceptProposedAction();
}

// Dropped files are booted together; dropped folders (when no files came along) become game
// list folders.
void MainWindow::dropEvent(QDropEvent* event)
{
  const QList<QUrl>& urls = event->mimeData()->urls();
  if (urls.empty())
    return;

  QStringList files;
  QStringList folders;
  for (const QUrl& url : urls)
  {
    const QFileInfo info(url.toLocalFile());
    const QString path = info.filePath();
    if (!info.exists() || !info.isReadable())
    {
      ModalMessageBox::critical(this, tr("Error"), tr("Failed to open '%1'").arg(path));
      return;
    }
    if (info.isFile())
      files.append(path);
    else
      folders.append(path);
  }

  if (files.isEmpty())
    AddDroppedFolders(folders);
  else
    StartGame(ToStdStrings(files));
}

void MainWindow::AddDroppedFolders(const QStringList& folders)
{
  Settings& settings = Settings::Instance();
  // Someone with no game folders yet obviously wants these; otherwise ask about each one.
  const bool ask_each = !settings.GetPaths().empty();

  for (const QString& folder : folders)
  {
    if (ask_each &&
        ModalMessageBox::question(
            this, tr("Confirm"),
            tr("Do you want to add \"%1\" to the list of Game Paths?").arg(folder)) !=
            QMessageBox::Yes)
    {
      return;
    }
    settings.AddPath(folder);
  }
}

QSize MainWindow::sizeHint() const
{
  return {800, 600};
}

void MainWindow::OnBootGameCubeIPL(DiscIO::Region region)
{
  StartGame(std::make_unique<BootParameters>(BootParameters::IPL{region}));
}

void MainWindow::OnImportNANDBackup()
{
  const auto go_ahead = ModalMessageBox::question(
      this, tr("Question"),
      tr("Merging a new NAND over your currently selected NAND will overwrite any channels "
         "and savegames that already exist. This process is not reversible, so it is "
         "recommended that you keep backups of both NANDs. Are you sure you want to "
         "continue?"));
  if (go_ahead == QMessageBox::No)
    return;

  const QString backup = DolphinFileDialog::getOpenFileName(
      this, tr("Select NAND Backup"), QDir::currentPath(),
      tr("BootMii NAND backup file (*.bin);;"
         "All Files (*)"));
  if (backup.isEmpty())
    return;

  ParallelProgressDialog progress(this);
  progress.GetRaw()->setWindowTitle(tr("Importing NAND backup"));

  // Reports a step of the import; returning true cancels it.
  const auto report = [&progress](DiscIO::NANDImporter::Step step, int done, int total) {
    progress.SetLabelText(step == DiscIO::NANDImporter::Step::Loading ?
                              tr("Loading NAND...") :
                              tr("Extracting NAND..."));
    progress.SetValue(done);
    progress.SetMaximum(total);
    return progress.WasCanceled();
  };

  // Backups without the console's keys appended need a separate keys file; the dialog for it
  // has to run on the GUI thread.
  const auto ask_for_keys = [this]() -> std::string {
    const std::optional<std::string> keys = RunOnObject(this, [this] {
      return DolphinFileDialog::getOpenFileName(this, tr("Select Keys File (OTP/SEEPROM Dump)"),
                                                QDir::currentPath(),
                                                tr("BootMii keys file (*.bin);;"
                                                   "All Files (*)"))
          .toStdString();
    });
    return keys.value_or(std::string());
  };

  std::future<void> import = std::async(std::launch::async, [&] {
    DiscIO::NANDImporter().ImportNANDBin(backup.toStdString(), report, ask_for_keys);
    progress.Reset();
  });

  progress.GetRaw()->exec();
  import.wait();

  m_menu_bar->UpdateToolsMenu(Core::State::Uninitialized);
}

void MainWindow::OnPlayRecording()
{
  if (AchievementManager::GetInstance().IsHardcoreModeActive())
  {
    ModalMessageBox::critical(
        this, tr("Error"),
        tr("Playback of input recordings is disabled in RetroAchievements hardcore mode."));
    return;
  }

  const QString movie_file = DolphinFileDialog::getOpenFileName(
      this, tr("Select the Recording File to Play"), QString(), tr("Dolphin TAS Movies (*.dtm)"));
  if (movie_file.isEmpty())
    return;

  Movie::MovieManager& movie = m_system.GetMovie();
  // Playback always starts out read-only.
  if (!movie.IsReadOnly())
  {
    movie.SetReadOnly(true);
    emit ReadOnlyModeChanged(true);
  }

  std::optional<std::string> movie_savestate;
  if (!movie.PlayInput(movie_file.toStdString(), &movie_savestate))
    return;

  emit RecordingStatusChanged(true);
  Play(movie_savestate);
}

void MainWindow::OnStartRecording()
{
  Movie::MovieManager& movie = m_system.GetMovie();
  const Core::State state = Core::GetState(m_system);
  const bool busy = state == Core::State::Starting || state == Core::State::Stopping ||
                    movie.IsRecordingInput() || movie.IsPlayingInput();
  if (busy)
    return;

  // Asking to record wins over read-only mode. (The signal still reports true, as it always
  // has.)
  if (movie.IsReadOnly())
  {
    movie.SetReadOnly(false);
    emit ReadOnlyModeChanged(true);
  }

  Movie::ControllerTypeArray pads{};
  Movie::WiimoteEnabledArray remotes{};
  for (int port = 0; port < PORT_COUNT; ++port)
  {
    const auto index = static_cast<size_t>(port);
    const SerialInterface::SIDevices device = Config::Get(Config::GetInfoForSIDevice(port));

    Movie::ControllerType type = Movie::ControllerType::None;
    if (device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      type = Movie::ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(device))
      type = Movie::ControllerType::GC;
    pads[index] = type;

    remotes[index] = Config::Get(Config::GetInfoForWiimoteSource(port)) != WiimoteSource::None;
  }

  if (!movie.BeginRecordingInput(pads, remotes))
    return;

  emit RecordingStatusChanged(true);
  if (Core::IsUninitialized(m_system))
    Play();
}

void MainWindow::OnStopRecording()
{
  Movie::MovieManager& movie = m_system.GetMovie();
  if (movie.IsRecordingInput())
    OnExportRecording();
  if (movie.IsMovieActive())
    movie.EndPlayInput(false);
  emit RecordingStatusChanged(false);
}

void MainWindow::OnExportRecording()
{
  // The emulated CPU stays parked while the user picks a file.
  const Core::CPUThreadGuard guard(m_system);

  const QString movie_file = DolphinFileDialog::getSaveFileName(
      this, tr("Save Recording File As"), QString(), tr("Dolphin TAS Movies (*.dtm)"));
  if (movie_file.isEmpty())
    return;
  m_system.GetMovie().SaveRecording(movie_file.toStdString());
}

void MainWindow::ToggleReadOnlyMode()
{
  Movie::MovieManager& movie = m_system.GetMovie();
  const bool read_only = !movie.IsReadOnly();
  movie.SetReadOnly(read_only);
  emit ReadOnlyModeChanged(read_only);
}

void MainWindow::OnActivateChat()
{
  if (g_netplay_chat_ui)
    g_netplay_chat_ui->Activate();
}

void MainWindow::OnRequestGolfControl()
{
  if (const auto client = Settings::Instance().GetNetPlayClient())
    client->RequestGolfControl();
}

// Opens the TAS input window of every port that has something plugged in.
void MainWindow::ShowTASInput()
{
  for (int port = 0; port < PORT_COUNT; ++port)
  {
    const auto index = static_cast<size_t>(port);
    const auto device = Config::Get(Config::GetInfoForSIDevice(port));
    if (device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      BringToFront(m_gba_tas_input_windows[index]);
    else if (device != SerialInterface::SIDEVICE_NONE &&
             device != SerialInterface::SIDEVICE_GC_GBA)
      BringToFront(m_gc_tas_input_windows[index]);
  }

  // Wii Remotes only count while no GameCube game is running.
  for (int remote = 0; remote < PORT_COUNT; ++remote)
  {
    const bool emulated =
        Config::Get(Config::GetInfoForWiimoteSource(remote)) == WiimoteSource::Emulated;
    if (emulated && (!Core::IsRunning(m_system) || m_system.IsWii()))
      BringToFront(m_wii_tas_input_windows[static_cast<size_t>(remote)]);
  }
}

void MainWindow::OnConnectWiiRemote(int id)
{
  const Core::CPUThreadGuard guard(m_system);
  const auto bluetooth = WiiUtils::GetBluetoothEmuDevice();
  if (!bluetooth)
    return;

  const auto remote = bluetooth->AccessWiimoteByIndex(static_cast<std::size_t>(id));
  remote->Activate(!remote->IsConnected());
}

#ifdef USE_RETRO_ACHIEVEMENTS
void MainWindow::ShowAchievementsWindow()
{
  Summon(m_achievements_window, [this] { return new AchievementsWindow(this); });
  m_achievements_window->UpdateData(AchievementManager::UpdatedItems{.all = true});
}

void MainWindow::ShowAchievementSettings()
{
  ShowAchievementsWindow();
  m_achievements_window->ForceSettingsTab();
}

void MainWindow::OnHardcoreChanged()
{
  const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
  if (hardcore)
    Settings::Instance().SetDebugModeEnabled(false);

  // Every window whose options depend on hardcore mode refreshes on EmulationStateChanged, so a
  // change of mode is announced as one.
  if (hardcore != m_hardcore_was_active)
    emit Settings::Instance().EmulationStateChanged(Core::GetState(Core::System::GetInstance()));
  m_hardcore_was_active = hardcore;
}
#endif  // USE_RETRO_ACHIEVEMENTS

void MainWindow::ShowMemcardManager()
{
  GCMemcardManager(this).exec();
}

void MainWindow::ShowResourcePackManager()
{
  ResourcePackManager(this).exec();
}

void MainWindow::ShowCheatsManager()
{
  if (m_cheats_manager == nullptr)
  {
    m_cheats_manager = new CheatsManager(m_system, this);
    connect(m_cheats_manager, &CheatsManager::ShowMemory, m_memory_widget,
            &MemoryWidget::SetAddress);
    connect(m_cheats_manager, &CheatsManager::RequestWatch, m_watch_widget,
            &WatchWidget::AddWatch);
    connect(m_cheats_manager, &CheatsManager::OpenGeneralSettings, this,
            &MainWindow::ShowGeneralWindow);
#ifdef USE_RETRO_ACHIEVEMENTS
    connect(m_cheats_manager, &CheatsManager::OpenAchievementSettings, this,
            &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  }
  m_cheats_manager->show();
}

void MainWindow::ShowRiivolutionBootWidget(const UICommon::GameFile& game)
{
  std::unique_ptr<BootParameters> boot = BootParameters::GenerateFromFile(DiscSetPaths(game));
  if (boot == nullptr)
    return;
  auto* const disc = std::get_if<BootParameters::Disc>(&boot->parameters);
  if (disc == nullptr)
    return;

  RiivolutionBootWidget chooser(disc->volume->GetGameID(), disc->volume->GetRevision(),
                                disc->volume->GetDiscNumber(), game.GetFilePath(), this);
#ifdef USE_RETRO_ACHIEVEMENTS
  connect(&chooser, &RiivolutionBootWidget::OpenAchievementSettings, this,
          &MainWindow::ShowAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS
  chooser.exec();
  if (!chooser.ShouldBoot())
    return;

  AddRiivolutionPatches(boot.get(), std::move(chooser.GetPatches()));
  StartGame(std::move(boot));
}
