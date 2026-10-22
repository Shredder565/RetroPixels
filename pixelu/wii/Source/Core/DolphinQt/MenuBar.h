// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <QMenuBar>
#include <QPointer>

#include "Common/CommonTypes.h"
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
#include "Common/HookableEvent.h"
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

class QMenu;
class ParallelProgressDialog;

namespace Core
{
enum class State;
}

namespace DiscIO
{
enum class Region;
}

namespace UICommon
{
class GameFile;
}

// An RSO module found in memory: the address of its header and its name.
using RSOPairEntry = std::pair<u32, std::string>;
using RSOVector = std::vector<RSOPairEntry>;

// The main window's menu bar. Most entries only emit a signal for MainWindow; the debugger's
// JIT and Symbols menus, the NAND and save tools and the game list view options are handled here.
class MenuBar final : public QMenuBar
{
  Q_OBJECT

public:
  static MenuBar* GetMenuBar() { return s_menu_bar; }

  explicit MenuBar(QWidget* parent = nullptr);

  void UpdateToolsMenu(Core::State state);
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  void UpdateAchievementDevelopmentMenu();
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

  QMenu* GetListColumnsMenu() const { return m_list_columns; }

  void InstallUpdateManually();

signals:
  // File
  void Open();
  void Exit();
  void ChangeDisc();
  void EjectDisc();
  void OpenUserFolder();
  void OpenConfigFolder();
  void OpenCacheFolder();

  // Emulation
  void Play();
  void Pause();
  void Stop();
  void Reset();
  void Fullscreen();
  void FrameAdvance();
  void Screenshot();
  void StartNetPlay();
  void BrowseNetPlay();
  void StateLoad();
  void StateSave();
  void StateLoadSlot();
  void StateSaveSlot();
  void StateLoadSlotAt(int slot);
  void StateSaveSlotAt(int slot);
  void StateLoadUndo();
  void StateSaveUndo();
  void StateSaveOldest();
  void SetStateSlot(int slot);
  void BootWiiSystemMenu();
  void ImportNANDBackup();

  void PerformOnlineUpdate(const std::string& region);

  // Tools
  void ShowMemcardManager();
  void BootGameCubeIPL(DiscIO::Region region);
  void ShowFIFOPlayer();
  void ShowAboutDialog();
  void ShowCheatsManager();
  void ShowResourcePackManager();
  void ShowSkylanderPortal();
  void ShowInfinityBase();
  void ShowWiiSpeakWindow();
  void ShowLogitechMicWindow();
  void ConnectWiiRemote(int id);

#ifdef USE_RETRO_ACHIEVEMENTS
  void ShowAchievementsWindow();
#endif  // USE_RETRO_ACHIEVEMENTS

  // Options
  void Configure();
  void ConfigureGraphics();
  void ConfigureAudio();
  void ConfigureControllers();
  void ConfigureHotkeys();
  void ConfigureFreelook();

  // View
  void ShowList();
  void ShowGrid();
  void PurgeGameListCache();
  void ShowSearch();
  void ColumnVisibilityToggled(const QString& row, bool visible);
  void GameListPlatformVisibilityToggled(const QString& row, bool visible);
  void GameListRegionVisibilityToggled(const QString& row, bool visible);

  // Movie
  void PlayRecording();
  void StartRecording();
  void StopRecording();
  void ExportRecording();
  void ShowTASInput();

  void SelectionChanged(const std::shared_ptr<const UICommon::GameFile>& game_file);
  void RecordingStatusChanged(bool recording);
  void ReadOnlyModeChanged(bool read_only);

  void ConfigureOSD();

private:
  // Building the menus
  void BuildFileMenu();
  void BuildEmulationMenu();
  void BuildSaveStateMenus(QMenu* emulation);
  void BuildMovieMenu();
  void BuildOptionsMenu();
  void BuildToolsMenu();
  void BuildViewMenu();
  void BuildDebuggerPanelToggles(QMenu* view);
  void BuildGameListViewChoice(QMenu* view);
  void BuildGameListFilterMenus(QMenu* view);
  void BuildJITMenu();
  void BuildSymbolsMenu();
  void BuildHelpMenu();

  // Keeping the menus current
  void ReflectEmulationState(Core::State state);
  void ReflectConfig();
  void ReflectDebugMode(bool enabled);
  void ReflectSelectedGame(const std::shared_ptr<const UICommon::GameFile>& game_file);
  void ReflectRecordingStatus(bool recording);
  void RelabelStateSlots();

  // Tools
  void InstallWAD();
  void ImportWiiSave();
  void ImportWiiSaves();
  void ExportWiiSaves();
  void CheckNAND();
  void NANDExtractCertificates();
  void ChangeDebugFont();

  // JIT
  void ClearJitCache();
  void WipeJitProfilingData();
  void WriteJitBlockLogDump();
  void SearchForInstruction();

  // Symbols
  void ClearSymbols();
  void GenerateSymbolsFromAddress();
  void GenerateSymbolsFromSignatureDB();
  void GenerateSymbolsFromRSO();
  void GenerateSymbolsFromRSOAuto();
  RSOVector DetectRSOModules(ParallelProgressDialog& progress);
  void LoadSymbolMap();
  void LoadMapFileChosenByUser(bool bad);
  void SaveSymbolMap();
  void SaveSymbolMapAs();
  void SaveCode();
  bool TryLoadMapFile(const QString& path, bool bad = false);
  void TrySaveSymbolMap(const QString& path);
  void CreateSignatureFile();
  void AppendSignatureFile();
  void ApplySignatureFile();
  void CombineSignatureFiles();
  void LoadRSOChain(u32 address, const QString& address_text);

  QString SignatureFileFilter() const;

  static QPointer<MenuBar> s_menu_bar;

  // File
  QAction* m_change_disc = nullptr;
  QAction* m_eject_disc = nullptr;

  // Emulation
  QAction* m_play = nullptr;
  QAction* m_pause = nullptr;
  QAction* m_stop = nullptr;
  QAction* m_reset = nullptr;
  QAction* m_fullscreen = nullptr;
  QAction* m_frame_advance = nullptr;
  QAction* m_screenshot = nullptr;
  QMenu* m_load_state = nullptr;
  QMenu* m_save_state = nullptr;
  QMenu* m_select_slot = nullptr;
  QMenu* m_load_from_slot = nullptr;
  QMenu* m_save_to_slot = nullptr;
  QActionGroup* m_slot_choices = nullptr;

  // Movie
  QAction* m_start_recording = nullptr;
  QAction* m_play_recording = nullptr;
  QAction* m_stop_recording = nullptr;
  QAction* m_export_recording = nullptr;
  QAction* m_read_only = nullptr;
  QAction* m_movie_window = nullptr;

  // Options (debug mode only)
  QAction* m_boot_to_pause = nullptr;
  QAction* m_reset_panic_ignore = nullptr;
  QAction* m_debug_font = nullptr;

  // Tools
  QAction* m_boot_system_menu = nullptr;
  QAction* m_install_wad = nullptr;
  QAction* m_import_nand_backup = nullptr;
  QAction* m_check_nand = nullptr;
  QMenu* m_online_update = nullptr;
  QAction* m_online_update_current_region = nullptr;
  std::array<QAction*, 4> m_gc_ipl{};  // NTSC-J, NTSC-U, PAL, Triforce
  std::array<QAction*, 5> m_wii_remotes{};  // remotes 1-4, balance board
  QAction* m_import_wii_save = nullptr;
  QAction* m_import_wii_saves = nullptr;
  QAction* m_export_wii_saves = nullptr;
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  QMenu* m_ra_development = nullptr;
  Common::EventHook m_ra_menu_hook;
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

  // View
  std::vector<QAction*> m_debugger_panels;
  QMenu* m_list_columns = nullptr;

  // JIT
  QMenu* m_jit = nullptr;
  QAction* m_jit_interpreter = nullptr;
  QAction* m_jit_no_block_linking = nullptr;
  QAction* m_jit_no_block_cache = nullptr;
  QAction* m_jit_no_fastmem_arena = nullptr;
  QAction* m_jit_no_large_entry_map = nullptr;
  QAction* m_jit_clear_cache = nullptr;
  QAction* m_jit_log_coverage = nullptr;
  QAction* m_jit_search = nullptr;
  QAction* m_jit_profiling = nullptr;
  QAction* m_jit_wipe_profiling = nullptr;
  QAction* m_jit_block_dump = nullptr;

  // Symbols
  QMenu* m_symbols = nullptr;

  bool m_game_selected = false;
  bool m_quiet_block_dump = false;
};
