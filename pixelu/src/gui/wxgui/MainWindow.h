// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/wx.h>
#include <wx/dataview.h>
#include <wx/infobar.h>

#include "wxgui/PadViewFrame.h"
#include "wxgui/MemorySearcherTool.h"

#include "config/XMLConfig.h"

#include "wxgui/LoggingWindow.h"
#include "wxgui/components/wxGameList.h"

#include <future>
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "Cafe/CafeSystem.h"

class DebuggerWindow2;
struct GameEntry;
class DiscordPresence;
class TitleManager;
class GraphicPacksWindow2;
class EmulatedUSBDeviceFrame;
class wxLaunchGameEvent;

wxDECLARE_EVENT(wxEVT_LAUNCH_GAME, wxLaunchGameEvent);
wxDECLARE_EVENT(wxEVT_SET_WINDOW_TITLE, wxCommandEvent);

// "start this game file", posted to the main window from wherever a launch is requested
class wxLaunchGameEvent : public wxCommandEvent
{
public:
	enum class INITIATED_BY
	{
		MENU,
		DRAG_AND_DROP,
		GAME_LIST,
		TITLE_MANAGER,
		COMMAND_LINE, // -g / -t
	};

	wxLaunchGameEvent(fs::path path, INITIATED_BY initiatedBy)
		: wxCommandEvent(wxEVT_LAUNCH_GAME), m_launchPath(path), m_initiatedBy(initiatedBy) {}

	[[nodiscard]] fs::path GetPath() const { return m_launchPath; }
	[[nodiscard]] INITIATED_BY GetInitiatedBy() const { return m_initiatedBy; }

	wxEvent* Clone() const { return new wxLaunchGameEvent(*this); }

private:
	fs::path m_launchPath;
	INITIATED_BY m_initiatedBy;
};

// The main window: menu bar, game list and, once a game runs, the TV picture. It also owns the
// optional GamePad window and the tool windows opened from the menu.
// Under --rp-embed it only ever shows the game picture (see RPEmbed.h).
class MainWindow : public wxFrame, public CafeSystem::SystemImplementation
{
	friend class CemuApp;

public:
	MainWindow();
	~MainWindow();

	// game list and info bar, shown while no game runs
	void CreateGameListAndStatusBar();
	void DestroyGameListAndStatusBar();

	void UpdateSettingsAfterGameLaunch();
	void RestoreSettingsAfterGameExited();

	// prepares and starts a game, an update folder or a standalone RPX/ELF; false (after telling the user) on failure
	bool FileLoad(const fs::path launchPath, wxLaunchGameEvent::INITIATED_BY initiatedBy);

	[[nodiscard]] bool IsGameLaunched() const { return m_game_launched; }

	void SetFullScreen(bool state);
	void EndEmulation();
	void SetMenuVisible(bool state);
	void UpdateNFCMenu();
	bool IsMenuHidden() const;
	void TogglePadView();
	void OpenPPCDebugger();

#if BOOST_OS_WINDOWS
	WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif
	// RetroPixels' screen switch ("PixelU.Screen", see RPEmbed.h): 0 = TV, 1 = GamePad, 2 = the other one,
	// anything else only asks. Returns whether the GamePad screen is shown now.
	static bool SetScreenMode(int mode);
	void OpenSettings();

	PadViewFrame* GetPadView() const { return m_padView; }

	// window events
	void OnSizeEvent(wxSizeEvent& event);
	void OnDPIChangedEvent(wxDPIChangedEvent& event);
	void OnMove(wxMoveEvent& event);
	void OnClose(wxCloseEvent& event);
	void OnDebuggerClose(wxCloseEvent& event);
	void OnPadClose(wxCloseEvent& event);
	void OnMemorySearcherClose(wxCloseEvent& event);

	// menu commands
	void OnFileMenu(wxCommandEvent& event);
	void OnOpenFolder(wxCommandEvent& event);
	void OnClearSpotPassCache(wxCommandEvent& event);
	void OnInstallUpdate(wxCommandEvent& event);
	void OnFileExit(wxCommandEvent& event);
	void OnNFCMenu(wxCommandEvent& event);
	void OnOptionsInput(wxCommandEvent& event);
	void OnAccountSelect(wxCommandEvent& event);
	void OnConsoleLanguage(wxCommandEvent& event);
	void OnHelpAbout(wxCommandEvent& event);
	void OnHelpUpdate(wxCommandEvent& event);
	void OnDebugSetting(wxCommandEvent& event);
	void OnDebugLoggingToggleFlagGeneric(wxCommandEvent& event);
	void OnPPCInfoToggle(wxCommandEvent& event);
	void OnDebugDumpGeneric(wxCommandEvent& event);
	void OnLoggingWindow(wxCommandEvent& event);
	void OnGDBStubToggle(wxCommandEvent& event);
	void OnDebugViewPPCThreads(wxCommandEvent& event);
	void OnDebugViewPPCDebugger(wxCommandEvent& event);
	void OnDebugViewAudioDebugger(wxCommandEvent& event);
	void OnDebugViewTextureRelations(wxCommandEvent& event);
	void OnToolsInput(wxCommandEvent& event);

	// requests from other windows and threads
	void OnLaunchFromFile(wxLaunchGameEvent& event);
	void OnGameListBeginUpdate(wxCommandEvent& event);
	void OnGameListEndUpdate(wxCommandEvent& event);
	void OnAccountListRefresh(wxCommandEvent& event);
	void OnRequestGameListRefresh(wxCommandEvent& event);
	void OnSetWindowTitle(wxCommandEvent& event);

	// input on the render canvas
	void OnMouseMove(wxMouseEvent& event);
	void OnMouseLeft(wxMouseEvent& event);
	void OnMouseRight(wxMouseEvent& event);
	void OnMouseWheel(wxMouseEvent& event);
	void OnKeyUp(wxKeyEvent& event);
	void OnKeyDown(wxKeyEvent& event);
	void OnChar(wxKeyEvent& event);
	void OnGesturePan(wxPanGestureEvent& event);

	void OnGameLoaded();

	// sets the title from any thread
	void AsyncSetTitle(std::string_view windowTitle);

	void CreateCanvas();
	void DestroyCanvas();

	static void ShowCursor(bool state);

	uintptr_t GetRenderCanvasHWND();

	// thread-safe requests to the main window
	static void RequestGameListRefresh();
	static void RequestLaunchGame(fs::path filePath, wxLaunchGameEvent::INITIATED_BY initiatedBy);

private:
	void BindEvents();
	bool FullscreenEnabled() const;
	void RecreateMenu();
	void UpdateChildWindowTitleRunningState();
	static wxString GetInitialWindowTitle();

	bool InstallUpdate(const fs::path& metaFilePath);

	void OnTimer(wxTimerEvent& event);

	// CafeSystem::SystemImplementation
	void CafeRecreateCanvas() override;
	void CafePPCProcessExit() override;

	void OnRequestRestart(wxCommandEvent& event);
	void OnRequestRecreateCanvas(wxCommandEvent& event);
	void OnRequestGameExit(wxCommandEvent& event);

	// geometry of the display the window is mostly on
	wxRect GetDesktopRect();

	void LoadSettings();
	void SaveSettings();

	void OnGraphicWindowClose(wxCloseEvent& event);
	void OnGraphicWindowOpen(wxTitleIdEvent& event);

	// tool windows (null while closed)
	MemorySearcherTool* m_toolWindow = nullptr;
	TitleManager* m_title_manager = nullptr;
	EmulatedUSBDeviceFrame* m_usb_devices = nullptr;
	PadViewFrame* m_padView = nullptr;
	GraphicPacksWindow2* m_graphic_pack_window = nullptr;
	DebuggerWindow2* m_debugger_window = nullptr;
	LoggingWindow* m_logging_window = nullptr;

	// cursor hiding in fullscreen and the update notice
	wxTimer* m_timer;
	wxPoint m_mouse_position{};
	std::chrono::steady_clock::time_point m_last_mouse_move_time;
	std::future<bool> m_update_available;

	// size and position while neither maximized nor fullscreen
	wxSize m_restored_size;
	wxPoint m_restored_position;

	bool m_menu_visible = false;
	bool m_game_launched = false;
	std::string m_launched_game_name;

#ifdef ENABLE_DISCORD_RPC
	std::unique_ptr<DiscordPresence> m_discord;
#endif

	// contents
	wxPanel* m_main_panel{};
	wxPanel* m_game_panel{};
	wxWindow* m_render_canvas{};
	wxGameList* m_game_list{};
	wxInfoBar* m_info_bar{};

	// menu bar and the items that change state
	wxMenuBar* m_menuBar{};
	wxMenu* m_fileMenu{};
	wxMenuItem* m_fileMenuSeparator0{};
	wxMenuItem* m_fileMenuSeparator1{};
	wxMenuItem* m_loadMenuItem{};
	wxMenuItem* m_installUpdateMenuItem{};
	wxMenuItem* m_exitMenuItem{};
	wxMenu* m_optionsAccountMenu{};
	wxMenuItem* m_fullscreenMenuItem{};
	wxMenuItem* m_padViewMenuItem{};
	wxMenuItem* m_memorySearcherMenuItem{};
	wxMenu* m_cpuTimerSubmenu{};
	wxMenu* m_nfcMenu{};
	wxMenuItem* m_nfcMenuSeparator0{};
	wxMenu* m_debugMenu{};
	wxMenu* m_loggingSubmenu{};
	wxMenuItem* m_asyncCompile{};
	wxMenuItem* m_gdbstub_toggle{};
	wxMenuItem* m_check_update_menu{};
};

extern MainWindow* g_mainFrame;
