// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QMainWindow>
#include <QStringList>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef USE_RETRO_ACHIEVEMENTS
#include "Common/Config/Config.h"
#endif  // USE_RETRO_ACHIEVEMENTS

#include "Core/Boot/Boot.h"
#include "DolphinQt/FIFO/FIFOPlayerWindow.h"

class QMenu;
class QStackedWidget;
class QString;

class AchievementsWindow;
class AssemblerWidget;
class BreakpointWidget;
struct BootParameters;
class CheatsManager;
class CodeWidget;
class DiscordHandler;
class FreeLookWindow;
class GameCount;
class GameList;
class GBATASInputWindow;
class GCTASInputWindow;
class HotkeyScheduler;
class InfinityBaseWindow;
class JITWidget;
class LogConfigWidget;
class LogWidget;
class LogitechMicWindow;
class MappingWindow;
class MemoryWidget;
class MenuBar;
class NetPlayDialog;
class NetPlaySetupDialog;
class NetworkWidget;
class RegisterWidget;
class RenderWidget;
class SearchBar;
class SettingsWindow;
class SkylanderPortalWindow;
class ThreadWidget;
class ToolBar;
class WatchWidget;
class WiiSpeakWindow;
class WiiTASInputWindow;
struct WindowSystemInfo;
enum class SettingsWindowPaneIndex : int;

namespace Core
{
class System;
}

namespace DiscIO
{
enum class Region;
}

namespace UICommon
{
class GameFile;
}

namespace X11Utils
{
class XRRConfiguration;
}

// The emulator's main window: the game list (or the game itself when rendering to the main
// window), the menu bar, the tool bar and the debugger docks. It owns the emulation session's
// life cycle: booting, pausing, stopping and the windows that belong to a session.
class MainWindow final : public QMainWindow
{
  Q_OBJECT

public:
  explicit MainWindow(Core::System& system, std::unique_ptr<BootParameters> boot_parameters,
                      const std::string& movie_path);
  ~MainWindow() override;

  WindowSystemInfo GetWindowSystemInfo() const;

  bool eventFilter(QObject* object, QEvent* event) override;
  QMenu* createPopupMenu() override;

  void ShowTriforceWindow();

signals:
  void ReadOnlyModeChanged(bool read_only);
  void RecordingStatusChanged(bool recording);

private:
  enum class ScanForSecondDisc
  {
    Yes,
    No,
  };

  // Building the window
  void CreateComponents();
  void CreateDebuggerWidgets();
  void LinkDebuggerWidgets();
  void ConnectGameList();
  void ConnectHost();
  void ConnectHotkeys();
  void ConnectMenuBar();
  void ConnectRenderWidget();
  void ConnectStack();
  void ConnectToolBar();
  void WatchRenderWidget();
  void InitControllers();
  void ShutdownControllers();
  void InitCoreCallbacks();
  bool CheckResourcePacks();

  // Session control
  void Open();
  void RefreshGameList();
  void Play(const std::optional<std::string>& savestate_path = {});
  void Pause();
  void TogglePause();
  // May ask the user first. Returns false only when the user chose to keep playing.
  bool RequestStop();
  enum class StopAnswer
  {
    Stop,
    KeepPlaying,
    AlreadyAsking,
  };
  StopAnswer AskBeforeStopping(QWidget* parent, bool render_widget_was_active,
                               bool was_cursor_locked);
  void ForceStop();
  void Reset();
  void FrameAdvance();
  void OnStopComplete();
  void BootWiiSystemMenu();
  void OnBootGameCubeIPL(DiscIO::Region region);
  void PerformOnlineUpdate(const std::string& region);
  void ChangeDisc();
  void EjectDisc();
  void OnConnectWiiRemote(int id);

  void ScanForSecondDiscAndStartGame(const UICommon::GameFile& game,
                                     std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(const QString& path, ScanForSecondDisc scan,
                 std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(const std::string& path, ScanForSecondDisc scan,
                 std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(const std::vector<std::string>& paths,
                 std::unique_ptr<BootSessionData> boot_session_data = nullptr);
  void StartGame(std::unique_ptr<BootParameters>&& parameters);
  bool ConfirmDiscCanBoot(const BootParameters& parameters);
  std::vector<std::string> DiscSetPaths(const UICommon::GameFile& game) const;

  // Save states
  void StateLoad();
  void StateSave();
  void StateLoadSlot();
  void StateSaveSlot();
  void StateLoadSlotAt(int slot);
  void StateSaveSlotAt(int slot);
  void StateLoadLastSavedAt(int slot);
  void StateLoadUndo();
  void StateSaveUndo();
  void StateSaveOldest();
  void SetStateSlot(int slot);
  void IncrementSelectedStateSlot();
  void DecrementSelectedStateSlot();
  QString StateFileDialogDirectory() const;

  // The render widget
  void FullScreen();
  void UnlockCursor();
  void ScreenShot();
  void SetFullScreenResolution(bool fullscreen);
  void ShowRenderWidget();
  void HideRenderWidget(bool reinit = true, bool is_exit = false);

  // Secondary windows
  void ShowSettingsWindow();
  void ShowSettingsPane(SettingsWindowPaneIndex pane);
  void ShowGeneralWindow();
  void ShowAudioWindow();
  void ShowControllersWindow();
  void ShowGraphicsWindow();
  void ShowOSDWindow();
  void ShowFreeLookWindow();
  void ShowAboutDialog();
  void ShowHotkeyDialog();
  void ShowNetPlaySetupDialog();
  void ShowNetPlayBrowser();
  void ShowFIFOPlayer();
  void ShowSkylanderPortal();
  void ShowInfinityBase();
  void ShowWiiSpeakWindow();
  void ShowLogitechMicWindow();
  void ShowMemcardManager();
  void ShowResourcePackManager();
  void ShowCheatsManager();
  void ShowRiivolutionBootWidget(const UICommon::GameFile& game);
  void ShowTASInput();
  void OnImportNANDBackup();

#ifdef USE_RETRO_ACHIEVEMENTS
  void ShowAchievementsWindow();
  void ShowAchievementSettings();
  void OnHardcoreChanged();
#endif  // USE_RETRO_ACHIEVEMENTS

  // NetPlay
  void NetPlayInit();
  bool NetPlayJoin();
  bool NetPlayHost(const UICommon::GameFile& game);
  void NetPlayQuit();
  bool NetPlayCanStart();
  void OnActivateChat();
  void OnRequestGolfControl();

  // Movies
  void OnPlayRecording();
  void OnStartRecording();
  void OnStopRecording();
  void OnExportRecording();
  void ToggleReadOnlyMode();

  // Files and folders
  QStringList PromptFileNames();
  void OpenUserFolder();
  void OpenConfigFolder();
  void OpenCacheFolder();
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void AddDroppedFolders(const QStringList& folders);

#if defined(__unix__) || defined(__unix) || defined(__APPLE__)
  void OnSignal();
#endif

  void UpdateScreenSaverInhibition();
  QSize sizeHint() const override;

  Core::System& m_system;

#ifdef HAVE_XRANDR
  std::unique_ptr<X11Utils::XRRConfiguration> m_xrr_config;
#endif

  // Always-present parts of the window
  QStackedWidget* m_stack = nullptr;
  ToolBar* m_tool_bar = nullptr;
  MenuBar* m_menu_bar = nullptr;
  SearchBar* m_search_bar = nullptr;
  GameList* m_game_list = nullptr;
  GameCount* m_game_count = nullptr;
  RenderWidget* m_render_widget = nullptr;
  HotkeyScheduler* m_hotkey_scheduler = nullptr;

  // Debugger docks
  AssemblerWidget* m_assembler_widget = nullptr;
  BreakpointWidget* m_breakpoint_widget = nullptr;
  CodeWidget* m_code_widget = nullptr;
  JITWidget* m_jit_widget = nullptr;
  LogWidget* m_log_widget = nullptr;
  LogConfigWidget* m_log_config_widget = nullptr;
  MemoryWidget* m_memory_widget = nullptr;
  NetworkWidget* m_network_widget = nullptr;
  RegisterWidget* m_register_widget = nullptr;
  ThreadWidget* m_thread_widget = nullptr;
  WatchWidget* m_watch_widget = nullptr;

  // Windows created the first time they are asked for
  SettingsWindow* m_settings_window = nullptr;
  // The FIFO player has no parent, so focusing it does not raise the main window; this pointer
  // owns it instead.
  std::unique_ptr<FIFOPlayerWindow> m_fifo_window;
  SkylanderPortalWindow* m_skylander_window = nullptr;
  InfinityBaseWindow* m_infinity_window = nullptr;
  WiiSpeakWindow* m_wii_speak_window = nullptr;
  LogitechMicWindow* m_logitech_mic_window = nullptr;
  MappingWindow* m_hotkey_window = nullptr;
  FreeLookWindow* m_freelook_window = nullptr;
  CheatsManager* m_cheats_manager = nullptr;

  // NetPlay
  NetPlayDialog* m_netplay_dialog = nullptr;
  DiscordHandler* m_netplay_discord = nullptr;
  NetPlaySetupDialog* m_netplay_setup_dialog = nullptr;

  // TAS input windows, one per port; they have no parent and are deleted by the destructor.
  static constexpr int PORT_COUNT = 4;
  std::array<GCTASInputWindow*, PORT_COUNT> m_gc_tas_input_windows{};
  std::array<GBATASInputWindow*, PORT_COUNT> m_gba_tas_input_windows{};
  std::array<WiiTASInputWindow*, PORT_COUNT> m_wii_tas_input_windows{};

#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementsWindow* m_achievements_window = nullptr;
  Config::ConfigChangedCallbackID m_config_changed_callback_id{};
  bool m_hardcore_was_active = false;
#endif  // USE_RETRO_ACHIEVEMENTS

  // Session state
  std::unique_ptr<BootParameters> m_pending_boot;
  QByteArray m_render_widget_geometry;
  u32 m_state_slot = 1;
  bool m_rendering_to_main = false;
  bool m_stop_confirm_showing = false;
  bool m_stop_requested = false;
  bool m_exit_requested = false;
  bool m_fullscreen_requested = false;
  bool m_is_screensaver_inhibited = false;
};
