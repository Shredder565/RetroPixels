// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "MainWindow.h"

// windows opened from the menus
#include "TitleManager.h"
#include "GeneralSettings2.h"
#include "GameUpdateWindow.h"
#include "CemuUpdateWindow.h"
#include "GraphicPacksWindow2.h"
#include "AudioDebuggerWindow.h"
#include "input/InputSettings2.h"
#include "input/HotkeySettings.h"
#include "debugger/DebuggerWindow2.h"
#include "EmulatedUSBDevices/EmulatedUSBDeviceFrame.h"
#include "windows/PPCThreadsViewer/DebugPPCThreadsWindow.h"
#include "windows/TextureRelationViewer/TextureRelationWindow.h"

#include "wxgui.h"
#include "wxCemuConfig.h"
#include "RPEmbed.h"
#include "interface/WindowSystem.h"
#include "wxHelper.h"
#include "helpers/wxHelpers.h"
#include "PadViewFrame.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif

#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "config/ActiveSettings.h"

#include "input/InputManager.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/DiscordPresence/DiscordPresence.h"
#include "util/ScreenSaver/ScreenSaver.h"
#include "util/helpers/SystemException.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VsyncDriver.h"
#if BOOST_OS_LINUX && defined(ENABLE_FERAL_GAMEMODE)
#include <gamemode_client.h>
#endif
#if (BOOST_OS_LINUX || BOOST_OS_BSD) && HAS_WAYLAND
#include "helpers/wxWayland.h"
#endif

#ifdef ENABLE_OPENGL
#include "canvas/OpenGLCanvas.h"
#endif
#ifdef ENABLE_VULKAN
#include "canvas/VulkanCanvas.h"
#endif

#include "Cafe/OS/libs/nfc/nfc.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/Latte.h" // LatteGPUState.isDRCPrimary (PixelU.Screen)

extern WindowSystem::WindowInfo g_window_info;
extern std::shared_mutex g_mutex;

wxDEFINE_EVENT(wxEVT_SET_WINDOW_TITLE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_REQUEST_GAMELIST_REFRESH, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_LAUNCH_GAME, wxLaunchGameEvent);
wxDEFINE_EVENT(wxEVT_REQUEST_RECREATE_CANVAS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_REQUEST_GAME_EXIT, wxCommandEvent);

namespace
{
	// Command ids. The numbers are fixed (translations, macOS menu roles and other windows rely on
	// the menu layout staying the same), so every block starts at an explicit value.
	namespace Cmd
	{
		enum : int
		{
			GameList = 20000,

			// File
			Load = 20100,
			InstallTitle = 20101,
			OpenUserFolder = 20102,
			OpenMlcFolder = 20103,
			OpenShaderCacheFolder = 20104,
			ClearSpotPass = 20105,
			Exit = 20106,
			CloseGame = 20107,
			RecentFirst = 20108,
			RecentLast = RecentFirst + 15,

			// Options
			Fullscreen = 20200,
			GamePadWindow = 20201,
			GraphicPacks = 20203,
			GeneralSettings = 20205,
			InputSettings = 20207,
			HotkeySettings = 20208,
			MacPreferences = 20209,

			AccountFirst = 20350,
			AccountLast = AccountFirst + 11,

			// Options > Console language; same order as the radio items' ids
			LangJapanese = 20500,
			LangEnglish,
			LangFrench,
			LangGerman,
			LangItalian,
			LangSpanish,
			LangChinese,
			LangKorean,
			LangDutch,
			LangPortuguese,
			LangRussian,
			LangTaiwanese,

			// Tools
			MemorySearcher = 20600,
			TitleManagerTab = 20601,
			DownloadManagerTab = 20602,
			UsbDevices = 20603,

			// CPU > Timer speed: 1x, 2x, 4x, 8x, 0.5x, 0.25x, 0.125x
			TimerSpeedFirst = 20700,
			TimerSpeedLast = 20706,

			// NFC
			TouchNfcFile = 21000,
			NfcRecentFirst = 21001,
			NfcRecentLast = NfcRecentFirst + 15,

			// Debug
			RenderUpsideDown = 21100,
			LoggingWindowCmd = 21101,
			GdbStub = 21102,
			PpcThreads = 21103,
			PpcDebugger = 21104,
			AudioDebugger = 21105,
			TextureRelations = 21106,
			AudioAuxOnly = 21107,
			AccurateBarriers = 21108,
			GpuCapture = 21109,

			// Debug > Logging: LogFirst + LogType
			LogExpertNote = 21499,
			LogFirst = 21500,
			LogPpcContext = 21599,

			// Debug > Dump
			DumpTextures = 21600,
			DumpShaders = 21601,
			DumpRecompiler = 21602,
			DumpRam = 21603,
			DumpRpx = 21604,
			DumpFst = 21605,
			DumpCurl = 21606,

			// Help
			About = 21700,
			CheckForUpdates = 21701,

			UiTimer = 21800,
		};
	}

	int LogCmd(LogType type)
	{
		return Cmd::LogFirst + (int)stdx::to_underlying(type);
	}

	// timer shift factor for each Timer speed entry, in id order (shift 3 is real time)
	constexpr int kTimerShiftForCmd[] = {3, 2, 1, 0, 4, 5, 6};

	struct LanguageEntry
	{
		int cmd;
		CafeConsoleLanguage language;
		const char* label;
	};

	// in menu order
	const LanguageEntry kConsoleLanguages[] = {
		{Cmd::LangEnglish, CafeConsoleLanguage::EN, wxTRANSLATE("&English")},
		{Cmd::LangJapanese, CafeConsoleLanguage::JA, wxTRANSLATE("&Japanese")},
		{Cmd::LangFrench, CafeConsoleLanguage::FR, wxTRANSLATE("&French")},
		{Cmd::LangGerman, CafeConsoleLanguage::DE, wxTRANSLATE("&German")},
		{Cmd::LangItalian, CafeConsoleLanguage::IT, wxTRANSLATE("&Italian")},
		{Cmd::LangSpanish, CafeConsoleLanguage::ES, wxTRANSLATE("&Spanish")},
		{Cmd::LangChinese, CafeConsoleLanguage::ZH, wxTRANSLATE("&Chinese")},
		{Cmd::LangKorean, CafeConsoleLanguage::KO, wxTRANSLATE("&Korean")},
		{Cmd::LangDutch, CafeConsoleLanguage::NL, wxTRANSLATE("&Dutch")},
		{Cmd::LangPortuguese, CafeConsoleLanguage::PT, wxTRANSLATE("&Portuguese")},
		{Cmd::LangRussian, CafeConsoleLanguage::RU, wxTRANSLATE("&Russian")},
		{Cmd::LangTaiwanese, CafeConsoleLanguage::TW, wxTRANSLATE("&Taiwanese")},
	};

	struct LogEntry
	{
		LogType type;
		const char* label;
	};

	const LogEntry kLogGeneral[] = {
		{LogType::UnsupportedAPI, wxTRANSLATE("&Unsupported API calls")},
		{LogType::APIErrors, wxTRANSLATE("&Invalid API usage")},
		{LogType::CoreinitLogging, wxTRANSLATE("&Coreinit Logging (OSReport/OSConsole)")},
	};

	const LogEntry kLogCafeModules[] = {
		{LogType::CoreinitFile, wxTRANSLATE("coreinit File-Access API")},
		{LogType::CoreinitThreadSync, wxTRANSLATE("coreinit Thread-Synchronization API")},
		{LogType::CoreinitMem, wxTRANSLATE("coreinit Memory API")},
		{LogType::CoreinitMP, wxTRANSLATE("coreinit MP API")},
		{LogType::CoreinitThread, wxTRANSLATE("coreinit Thread API")},
		{LogType::Save, wxTRANSLATE("nn_save API")},
		{LogType::NN_NFP, wxTRANSLATE("nn_nfp API")},
		{LogType::NN_FP, wxTRANSLATE("nn_fp API")},
		{LogType::PRUDP, wxTRANSLATE("nn_fp PRUDP")},
		{LogType::NN_BOSS, wxTRANSLATE("nn_boss API")},
		{LogType::NFC, wxTRANSLATE("nfc API")},
		{LogType::NTAG, wxTRANSLATE("ntag API")},
		{LogType::Socket, wxTRANSLATE("nsysnet API")},
		{LogType::H264, wxTRANSLATE("h264 API")},
		{LogType::GX2, wxTRANSLATE("gx2 API")},
		{LogType::SWKBD, wxTRANSLATE("swkbd API")},
		{LogType::SoundAPI, wxTRANSLATE("Audio API")},
		{LogType::InputAPI, wxTRANSLATE("Input API")},
	};

	const LogEntry kLogGraphics[] = {
		{LogType::Patches, wxTRANSLATE("&Graphic pack patches")},
		{LogType::TextureCache, wxTRANSLATE("&Texture cache warnings")},
		{LogType::TextureReadback, wxTRANSLATE("&Texture readback")},
	};

	const LogEntry kLogRenderer[] = {
		{LogType::OpenGLLogging, wxTRANSLATE("&OpenGL debug output")},
		{LogType::VulkanValidation, wxTRANSLATE("&Vulkan validation layer (slow)")},
	};

	template<size_t N>
	void AppendLogItems(wxMenu* menu, const LogEntry (&entries)[N])
	{
		for (const LogEntry& entry : entries)
			menu->AppendCheckItem(LogCmd(entry.type), wxGetTranslation(entry.label))->Check(cemuLog_isLoggingEnabled(entry.type));
	}

	// NFC touch result -> message for the user (empty when there is nothing to say)
	wxString NfcErrorText(uint32 nfcError)
	{
		if (nfcError == NFC_TOUCH_TAG_ERROR_NO_ACCESS)
			return _("Cannot open file");
		if (nfcError == NFC_TOUCH_TAG_ERROR_INVALID_FILE_FORMAT)
			return _("Not a valid NFC file");
		return {};
	}

	// a single game file dropped on the game list starts it
	class GameFileDrop : public wxFileDropTarget
	{
	public:
		explicit GameFileDrop(MainWindow* owner) : m_owner(owner) {}

		bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& files) override
		{
			if (m_owner->IsGameLaunched() || files.GetCount() != 1)
				return false;
			return m_owner->FileLoad(_utf8ToPath(files[0].utf8_string()), wxLaunchGameEvent::INITIATED_BY::DRAG_AND_DROP);
		}

	private:
		MainWindow* m_owner;
	};

	// a single NFC/amiibo file dropped on the running game is scanned
	class NfcFileDrop : public wxFileDropTarget
	{
	public:
		explicit NfcFileDrop(MainWindow* owner) : m_owner(owner) {}

		bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& files) override
		{
			if (!m_owner->IsGameLaunched() || files.GetCount() != 1)
				return false;
			const std::string file = files[0].utf8_string();
			uint32 nfcError;
			if (!nfc::TouchTagFromFile(_utf8ToPath(file), &nfcError))
			{
				const wxString text = NfcErrorText(nfcError);
				if (!text.empty())
					wxMessageBox(text, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
				return false;
			}
			GetWxGUIConfig().AddRecentNfcFile(file);
			m_owner->UpdateNFCMenu();
			return true;
		}

	private:
		MainWindow* m_owner;
	};

	void ShowLaunchError(const wxString& text)
	{
		RPEmbed::ShowMessage(text, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
	}

	// keeps g_window_info's view of the client area current
	void PublishClientArea(wxWindow* window)
	{
		const wxSize area = window->GetClientSize();
		g_window_info.width = area.GetWidth();
		g_window_info.height = area.GetHeight();
		g_window_info.phys_width = window->ToPhys(area.GetWidth());
		g_window_info.phys_height = window->ToPhys(area.GetHeight());
		g_window_info.dpi_scale = window->GetDPIScaleFactor();
	}
}

MainWindow::MainWindow()
	: wxFrame(nullptr, wxID_ANY, GetInitialWindowTitle(), wxDefaultPosition, wxSize(1280, 720),
			  RPEmbed::FrameStyle(wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxCLIP_CHILDREN | wxRESIZE_BORDER))
{
	// before anything below can produce an event
	BindEvents();

#ifdef __WXMAC__
	wxApp::s_macAboutMenuItemId = Cmd::About;
	wxApp::s_macPreferencesMenuItemId = Cmd::MacPreferences;
#endif
	g_window_info.window_main = initHandleContextFromWxWidgetsWindow(this);
	g_mainFrame = this;
	CafeSystem::SetImplementation(this);

	RecreateMenu();
	SetClientSize(1280, 720);
	SetIcon(wxICON(M_WND_ICON128));
#if BOOST_OS_MACOS
	EnableFullScreenView(true);
#endif
#if BOOST_OS_WINDOWS
	const HICON smallIcon = (HICON)LoadImageA(nullptr, "M_WND_ICON16", IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
	SendMessage(GetHWND(), WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
#endif

	// -g / -t start a game straight away; the game list is then never set up
	bool startingGame = false;
	if (const auto file = LaunchSettings::GetLoadFile())
	{
		RequestLaunchGame(*file, wxLaunchGameEvent::INITIATED_BY::COMMAND_LINE);
		startingGame = true;
	}
	else if (const auto titleId = LaunchSettings::GetLoadTitleID())
	{
		TitleId baseTitle;
		TitleInfo title;
		if (CafeTitleList::FindBaseTitleId(*titleId, baseTitle) && CafeTitleList::GetFirstByTitleId(baseTitle, title))
		{
			RequestLaunchGame(title.GetPath(), wxLaunchGameEvent::INITIATED_BY::COMMAND_LINE);
			startingGame = true;
		}
		else
		{
			ShowLaunchError(wxString(fmt::format("Title ID {:016x} not found", *titleId)));
		}
	}

	SetSizer(new wxBoxSizer(wxVERTICAL));
	if (startingGame)
	{
		m_game_list = nullptr;
		m_info_bar = nullptr;
	}
	else
		CreateGameListAndStatusBar();

	m_last_mouse_move_time = std::chrono::steady_clock::now();
	m_timer = new wxTimer(this, Cmd::UiTimer);
	m_timer->Start(500);

	LoadSettings();

#ifdef ENABLE_DISCORD_RPC
	if (GetWxGUIConfig().use_discord_presence)
		m_discord = std::make_unique<DiscordPresence>();
#endif

	if (LaunchSettings::GDBStubEnabled())
		g_gdbstub = std::make_unique<GDBServer>(GetConfig().gdb_port);

	if (LaunchSettings::OpenDebuggerEnabled())
		OpenPPCDebugger();
}

void MainWindow::BindEvents()
{
	Bind(wxEVT_TIMER, &MainWindow::OnTimer, this, Cmd::UiTimer);
	Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnClose, this);
	Bind(wxEVT_SIZE, &MainWindow::OnSizeEvent, this);
	Bind(wxEVT_DPI_CHANGED, &MainWindow::OnDPIChangedEvent, this);
	Bind(wxEVT_MOVE, &MainWindow::OnMove, this);

	auto onMenu = [this](void (MainWindow::*handler)(wxCommandEvent&), int first, int last) {
		Bind(wxEVT_MENU, handler, this, first, last);
	};

	// File
	onMenu(&MainWindow::OnFileMenu, Cmd::Load, Cmd::Load);
	onMenu(&MainWindow::OnFileMenu, Cmd::CloseGame, Cmd::CloseGame);
	onMenu(&MainWindow::OnFileMenu, Cmd::RecentFirst, Cmd::RecentLast);
	onMenu(&MainWindow::OnInstallUpdate, Cmd::InstallTitle, Cmd::InstallTitle);
	onMenu(&MainWindow::OnOpenFolder, Cmd::OpenUserFolder, Cmd::OpenShaderCacheFolder);
	onMenu(&MainWindow::OnClearSpotPassCache, Cmd::ClearSpotPass, Cmd::ClearSpotPass);
	onMenu(&MainWindow::OnFileExit, Cmd::Exit, Cmd::Exit);

	// Options
	onMenu(&MainWindow::OnAccountSelect, Cmd::AccountFirst, Cmd::AccountLast);
	onMenu(&MainWindow::OnConsoleLanguage, Cmd::LangJapanese, Cmd::LangTaiwanese);
	onMenu(&MainWindow::OnOptionsInput, Cmd::Fullscreen, Cmd::MacPreferences);

	// Tools
	onMenu(&MainWindow::OnToolsInput, Cmd::MemorySearcher, Cmd::UsbDevices);

	// CPU
	onMenu(&MainWindow::OnDebugSetting, Cmd::TimerSpeedFirst, Cmd::TimerSpeedLast);

	// NFC
	onMenu(&MainWindow::OnNFCMenu, Cmd::TouchNfcFile, Cmd::NfcRecentLast);

	// Debug
	onMenu(&MainWindow::OnDebugLoggingToggleFlagGeneric, Cmd::LogFirst, Cmd::LogFirst + 98);
	onMenu(&MainWindow::OnPPCInfoToggle, Cmd::LogPpcContext, Cmd::LogPpcContext);
	onMenu(&MainWindow::OnDebugDumpGeneric, Cmd::DumpTextures, Cmd::DumpRecompiler);
	onMenu(&MainWindow::OnDebugSetting, Cmd::DumpRam, Cmd::DumpCurl);
	onMenu(&MainWindow::OnDebugSetting, Cmd::RenderUpsideDown, Cmd::RenderUpsideDown);
	onMenu(&MainWindow::OnDebugSetting, Cmd::AudioAuxOnly, Cmd::GpuCapture);
	onMenu(&MainWindow::OnLoggingWindow, Cmd::LoggingWindowCmd, Cmd::LoggingWindowCmd);
	onMenu(&MainWindow::OnGDBStubToggle, Cmd::GdbStub, Cmd::GdbStub);
	onMenu(&MainWindow::OnDebugViewPPCThreads, Cmd::PpcThreads, Cmd::PpcThreads);
	onMenu(&MainWindow::OnDebugViewPPCDebugger, Cmd::PpcDebugger, Cmd::PpcDebugger);
	onMenu(&MainWindow::OnDebugViewAudioDebugger, Cmd::AudioDebugger, Cmd::AudioDebugger);
	onMenu(&MainWindow::OnDebugViewTextureRelations, Cmd::TextureRelations, Cmd::TextureRelations);

	// Help
	onMenu(&MainWindow::OnHelpAbout, Cmd::About, Cmd::About);
	onMenu(&MainWindow::OnHelpUpdate, Cmd::CheckForUpdates, Cmd::CheckForUpdates);

	// requests from the game list, other windows and other threads
	Bind(wxEVT_REQUEST_GAMELIST_REFRESH, &MainWindow::OnRequestGameListRefresh, this);
	Bind(wxEVT_GAMELIST_BEGIN_UPDATE, &MainWindow::OnGameListBeginUpdate, this);
	Bind(wxEVT_GAMELIST_END_UPDATE, &MainWindow::OnGameListEndUpdate, this);
	Bind(wxEVT_ACCOUNTLIST_REFRESH, &MainWindow::OnAccountListRefresh, this);
	Bind(wxEVT_SET_WINDOW_TITLE, &MainWindow::OnSetWindowTitle, this);
	Bind(wxEVT_REQUEST_RECREATE_CANVAS, &MainWindow::OnRequestRecreateCanvas, this);
	Bind(wxEVT_REQUEST_GAME_EXIT, &MainWindow::OnRequestGameExit, this);
	Bind(wxEVT_OPEN_GRAPHIC_PACK, &MainWindow::OnGraphicWindowOpen, this);
	Bind(wxEVT_LAUNCH_GAME, &MainWindow::OnLaunchFromFile, this);
}

MainWindow::~MainWindow()
{
	if (m_padView)
	{
		m_padView->Destroy();
		m_padView = nullptr;
	}
	m_timer->Stop();

	std::unique_lock guard(g_mutex);
	g_mainFrame = nullptr;
}

wxString MainWindow::GetInitialWindowTitle()
{
	return RPEmbed::Enabled() ? wxString(EMULATOR_NAME) : wxString(BUILD_VERSION_WITH_NAME_STRING);
}

bool MainWindow::FullscreenEnabled() const
{
	if (RPEmbed::Enabled())
		return false; // docked: fullscreen belongs to RetroPixels' window
	return LaunchSettings::FullscreenEnabled().value_or(GetWxGUIConfig().fullscreen);
}

// ---- game list ------------------------------------------------------------------------------

void MainWindow::CreateGameListAndStatusBar()
{
	if (m_main_panel || RPEmbed::Enabled()) // shown already / docked windows only show the game
		return;

	m_main_panel = new wxPanel(this);
	auto* column = new wxBoxSizer(wxVERTICAL);

	m_game_list = new wxGameList(m_main_panel, Cmd::GameList);
	m_game_list->Bind(wxEVT_OPEN_SETTINGS, [this](wxCommandEvent&) { OpenSettings(); });
	m_game_list->SetDropTarget(new GameFileDrop(this));
	column->Add(m_game_list, 1, wxEXPAND);

	m_info_bar = new wxInfoBar(m_main_panel);
	m_info_bar->SetShowHideEffects(wxSHOW_EFFECT_BLEND, wxSHOW_EFFECT_BLEND);
	m_info_bar->SetEffectDuration(500);
	column->Add(m_info_bar, 0, wxALL | wxEXPAND, 5);

	m_main_panel->SetSizer(column);
	GetSizer()->Add(m_main_panel, 1, wxEXPAND);
}

void MainWindow::DestroyGameListAndStatusBar()
{
	if (!m_main_panel)
		return;
	m_main_panel->Destroy();
	m_main_panel = nullptr;
	m_game_list = nullptr;
	m_info_bar = nullptr;
}

void MainWindow::OnGameListBeginUpdate(wxCommandEvent&)
{
	if (m_game_list && m_info_bar && m_game_list->IsShown())
		m_info_bar->ShowMessage(_("Updating game list..."));
}

void MainWindow::OnGameListEndUpdate(wxCommandEvent&)
{
	if (m_info_bar)
		m_info_bar->Dismiss();
}

void MainWindow::OnRequestGameListRefresh(wxCommandEvent&)
{
	if (m_game_list)
		m_game_list->ReloadGameEntries();
}

void MainWindow::OnAccountListRefresh(wxCommandEvent&)
{
	RecreateMenu();
}

void MainWindow::RequestGameListRefresh()
{
	wxQueueEvent(g_mainFrame, new wxCommandEvent(wxEVT_REQUEST_GAMELIST_REFRESH));
}

// ---- starting and stopping games -----------------------------------------------------------------

void MainWindow::RequestLaunchGame(fs::path filePath, wxLaunchGameEvent::INITIATED_BY initiatedBy)
{
	wxLaunchGameEvent launch(filePath, initiatedBy);
	wxPostEvent(g_mainFrame, launch);
}

void MainWindow::OnLaunchFromFile(wxLaunchGameEvent& event)
{
	if (event.GetPath().empty())
		return;
	const bool started = FileLoad(event.GetPath(), event.GetInitiatedBy());
	// docked into RetroPixels there is nothing else to show (the error was already reported)
	if (!started && RPEmbed::Enabled())
		Close(true);
}

bool MainWindow::FileLoad(const fs::path launchPath, wxLaunchGameEvent::INITIATED_BY initiatedBy)
{
	const wxString pathText = wxString::FromUTF8(_pathToUtf8(launchPath));
	TitleInfo title{launchPath};

	if (title.IsValid())
	{
		// may not be in the title list yet (e.g. opened from a file dialog)
		CafeTitleList::AddTitleFromPath(launchPath);
		TitleId baseTitle;
		if (!CafeTitleList::FindBaseTitleId(title.GetAppTitleId(), baseTitle))
		{
			ShowLaunchError(_("Unable to launch game because the base files were not found."));
			return false;
		}
		switch (CafeSystem::PrepareForegroundTitle(baseTitle))
		{
		case CafeSystem::PREPARE_STATUS_CODE::SUCCESS:
			break;
		case CafeSystem::PREPARE_STATUS_CODE::INVALID_RPX:
			cemu_assert_debug(false);
			return false;
		case CafeSystem::PREPARE_STATUS_CODE::UNABLE_TO_MOUNT:
			ShowLaunchError(_("Unable to mount title.\nMake sure the configured game paths are still valid and refresh the game list.\n\nFile which failed to load:\n") + pathText);
			return false;
		default:
			ShowLaunchError(_("Failed to launch game.") + pathText);
			return false;
		}
	}
	else
	{
		const CafeTitleFileType fileType = DetermineCafeSystemFileType(launchPath);
		if (fileType == CafeTitleFileType::RPX || fileType == CafeTitleFileType::ELF)
		{
			if (CafeSystem::PrepareForegroundTitleFromStandaloneRPX(launchPath) != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
			{
				cemu_assert_debug(false);
				ShowLaunchError(_("Failed to launch executable. Path: ") + pathText);
				return false;
			}
		}
		else if (initiatedBy == wxLaunchGameEvent::INITIATED_BY::GAME_LIST)
		{
			ShowLaunchError(_("Unable to launch title.\nMake sure the configured game paths are still valid and refresh the game list.\n\nPath which failed to load:\n") + pathText);
			return false;
		}
		else
		{
			wxString text = _("Unable to launch game\nPath:\n") + pathText;
			const auto reason = title.GetInvalidReason();
			if (reason == TitleInfo::InvalidReason::NO_DISC_KEY)
				text << "\n\n" << _("Could not decrypt title. Make sure that keys.txt contains the correct disc key for this title.");
			if (reason == TitleInfo::InvalidReason::NO_TITLE_TIK)
				text << "\n\n" << _("Could not decrypt title because title.tik is missing.");
			ShowLaunchError(text);
			return false;
		}
	}

	GetWxGUIConfig().AddRecentlyLaunchedFile(_pathToUtf8(title.IsValid() ? title.GetPath() : launchPath));

	wxWindowUpdateLocker freeze(this);
	DestroyGameListAndStatusBar();

	m_game_launched = true;
	// loading and installing are only possible while no game runs
	m_memorySearcherMenuItem->Enable(true);
	m_installUpdateMenuItem->Enable(false);
	m_loadMenuItem->Enable(false);

	m_launched_game_name = CafeSystem::GetForegroundTitleName();
#ifdef ENABLE_DISCORD_RPC
	if (m_discord)
		m_discord->UpdatePresence(DiscordPresence::Playing, m_launched_game_name);
#endif

	if (FullscreenEnabled())
		SetFullScreen(true);
	if (GetConfig().disable_screensaver)
		ScreenSaver::SetInhibit(true);

#if BOOST_OS_LINUX && defined(ENABLE_FERAL_GAMEMODE)
	if (GetWxGUIConfig().feral_gamemode)
	{
		if (gamemode_request_start() < 0)
			cemuLog_log(LogType::Force, "Could not start GameMode");
		else
			cemuLog_log(LogType::Force, "GameMode has been started.");
	}
#endif

	CreateCanvas();
	CafeSystem::LaunchForegroundTitle();
	RecreateMenu();
	UpdateChildWindowTitleRunningState();
	return true;
}

void MainWindow::EndEmulation()
{
	CafeSystem::ShutdownTitle();
	DestroyCanvas();
	m_game_launched = false;
	m_launched_game_name.clear();
#ifdef ENABLE_DISCORD_RPC
	if (m_discord)
		m_discord->UpdatePresence(DiscordPresence::Idling, "");
#endif
	if (GetConfig().disable_screensaver)
		ScreenSaver::SetInhibit(false);

	if (m_toolWindow)
	{
		m_toolWindow->Close();
		m_toolWindow = nullptr;
		m_memorySearcherMenuItem->Enable(false);
	}

	RecreateMenu();
	CreateGameListAndStatusBar();
	DoLayout();
	UpdateChildWindowTitleRunningState();
}

void MainWindow::OnGameLoaded()
{
	if (m_debugger_window)
		m_debugger_window->OnGameLoaded();
}

void MainWindow::UpdateSettingsAfterGameLaunch()
{
	m_update_available = {};
	RecreateMenu();
}

void MainWindow::RestoreSettingsAfterGameExited()
{
	RecreateMenu();
}

void MainWindow::UpdateChildWindowTitleRunningState()
{
	if (m_graphic_pack_window)
		m_graphic_pack_window->UpdateTitleRunning(CafeSystem::IsTitleRunning());
}

void MainWindow::CafePPCProcessExit()
{
	// runs on the emulated CPU thread
	wxQueueEvent(g_mainFrame, new wxCommandEvent(wxEVT_REQUEST_GAME_EXIT));
}

void MainWindow::OnRequestGameExit(wxCommandEvent&)
{
	// started from the command line: quit (CemuApp::OnExit returns the title's exit code);
	// started from the game list: go back to it
	if (LaunchSettings::GetLoadFile() || LaunchSettings::GetLoadTitleID())
		Close();
	else
		EndEmulation();
}

void MainWindow::CafeRecreateCanvas()
{
	// called from the GPU thread; the canvas must be rebuilt on the UI thread, so wait for it there
	CounterSemaphore done;
	auto* request = new wxCommandEvent(wxEVT_REQUEST_RECREATE_CANVAS);
	request->SetClientData(&done);
	wxQueueEvent(g_mainFrame, request);
	done.decrementWithWait();
}

void MainWindow::OnRequestRecreateCanvas(wxCommandEvent& event)
{
	auto* done = static_cast<CounterSemaphore*>(event.GetClientData());
	DestroyCanvas();
	CreateCanvas();
	done->increment();
}

// ---- render canvas -------------------------------------------------------------------------------

void MainWindow::CreateCanvas()
{
	m_game_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxNO_BORDER | wxWANTS_CHARS);
	m_game_panel->Bind(wxEVT_KEY_UP, &MainWindow::OnKeyUp, this);
	m_game_panel->Bind(wxEVT_CHAR, &MainWindow::OnChar, this);
	m_game_panel->SetSizer(new wxBoxSizer(wxVERTICAL));
	GetSizer()->Add(m_game_panel, 1, wxEXPAND);

	const wxSize canvasSize(1280, 720);
	const auto api = ActiveSettings::GetGraphicsAPI();
#ifdef ENABLE_OPENGL
	if (api == kOpenGL)
		m_render_canvas = GLCanvas_Create(m_game_panel, canvasSize, true);
#endif
#ifdef ENABLE_VULKAN
	if (api == kVulkan)
		m_render_canvas = new VulkanCanvas(m_game_panel, canvasSize, true);
#endif
	if (!m_render_canvas)
		cemu_assert(false && "no canvas for the selected graphics API");
	cemu_assert(m_render_canvas != nullptr);

	m_render_canvas->Bind(wxEVT_MOTION, &MainWindow::OnMouseMove, this);
	m_render_canvas->Bind(wxEVT_MOUSEWHEEL, &MainWindow::OnMouseWheel, this);
	for (const auto& type : {wxEVT_LEFT_DOWN, wxEVT_LEFT_UP})
		m_render_canvas->Bind(type, &MainWindow::OnMouseLeft, this);
	for (const auto& type : {wxEVT_RIGHT_DOWN, wxEVT_RIGHT_UP})
		m_render_canvas->Bind(type, &MainWindow::OnMouseRight, this);
	m_render_canvas->Bind(wxEVT_GESTURE_PAN, &MainWindow::OnGesturePan, this);
	m_render_canvas->Bind(wxEVT_KEY_UP, &MainWindow::OnKeyUp, this);
	m_render_canvas->Bind(wxEVT_KEY_DOWN, &MainWindow::OnKeyDown, this);
	m_render_canvas->Bind(wxEVT_CHAR, &MainWindow::OnChar, this);
	m_render_canvas->SetDropTarget(new NfcFileDrop(this));

	m_game_panel->GetSizer()->Add(m_render_canvas, 1, wxEXPAND);
	GetSizer()->Layout();
	m_render_canvas->SetFocus();

	if (m_padView)
		m_padView->InitializeRenderCanvas();
}

void MainWindow::DestroyCanvas()
{
	if (m_padView)
		m_padView->DestroyCanvas();
	if (m_render_canvas)
	{
		m_render_canvas->Destroy();
		m_render_canvas = nullptr;
	}
	if (m_game_panel)
	{
		m_game_panel->Destroy();
		m_game_panel = nullptr;
	}
}

uintptr_t MainWindow::GetRenderCanvasHWND()
{
#if BOOST_OS_WINDOWS
	return m_render_canvas ? (uintptr_t)m_render_canvas->GetHWND() : 0;
#else
	return 0;
#endif
}

// ---- window events -------------------------------------------------------------------------------

void MainWindow::OnClose(wxCloseEvent& event)
{
	if (m_debugger_window)
	{
		m_debugger_window->CleanupForDestroy();
		m_debugger_window->Destroy();
		m_debugger_window = nullptr;
	}
	if (m_game_list)
		m_game_list->OnClose(event);

	if (!IsMaximized() && !WindowSystem::IsFullScreen())
		m_restored_size = GetSize();
	SaveSettings();
	m_timer->Stop();

	event.Skip();

	CafeSystem::Shutdown();
	DestroyCanvas();
}

void MainWindow::OnSizeEvent(wxSizeEvent& event)
{
	if (!IsMaximized() && !WindowSystem::IsFullScreen())
		m_restored_size = GetSize();
	PublishClientArea(this);

	if (m_debugger_window && m_debugger_window->IsShown())
		m_debugger_window->OnParentMove(GetPosition(), event.GetSize());

	event.Skip();
#ifdef ENABLE_VULKAN
	VsyncDriver_notifyWindowPosChanged();
#endif
}

void MainWindow::OnDPIChangedEvent(wxDPIChangedEvent& event)
{
	event.Skip();
	PublishClientArea(this);
}

void MainWindow::OnMove(wxMoveEvent&)
{
	if (!IsMaximized() && !WindowSystem::IsFullScreen())
		m_restored_position = GetPosition();

	if (m_debugger_window && m_debugger_window->IsShown())
		m_debugger_window->OnParentMove(GetPosition(), GetSize());
#ifdef ENABLE_VULKAN
	VsyncDriver_notifyWindowPosChanged();
#endif
}

void MainWindow::OnTimer(wxTimerEvent&)
{
	if (m_update_available.valid() && future_is_ready(m_update_available))
	{
		if (m_update_available.get())
		{
			wxMessageDialog ask(this, _("There's a new update available.\nDo you want to update?"), _("Update notification"), wxCENTRE | wxYES_NO);
			if (ask.ShowModal() == wxID_YES)
			{
				CemuUpdateWindow updater(this);
				updater.ShowModal();
			}
		}
		m_update_available = {};
	}

	// in fullscreen (menu hidden) the cursor disappears after 3s without movement
	if (!IsFullScreen() || m_menu_visible)
		return;

	const wxPoint now = wxGetMousePosition();
	if (now != m_mouse_position)
	{
		m_mouse_position = now;
		m_last_mouse_move_time = std::chrono::steady_clock::now();
		ShowCursor(true);
		return;
	}
	const auto stillFor = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_last_mouse_move_time);
	if (stillFor.count() > 3000)
		ShowCursor(false);
}

#if BOOST_OS_WINDOWS
WXLRESULT MainWindow::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam)
{
	// "PixelU.Screen" from RetroPixels picks the screen this window shows:
	// wParam 0 = TV, 1 = GamePad, 2 = the other one, anything else = just ask.
	// The reply is 1 while the GamePad screen is shown, otherwise 0.
	static const UINT screenMessage = RegisterWindowMessageW(L"PixelU.Screen");
	if (screenMessage != 0 && nMsg == screenMessage)
	{
		bool& gamePadShown = LatteGPUState.isDRCPrimary;
		switch (wParam)
		{
		case 0: gamePadShown = false; break;
		case 1: gamePadShown = true; break;
		case 2: gamePadShown = !gamePadShown; break;
		default: break;
		}
		return gamePadShown ? 1 : 0;
	}

	constexpr WPARAM kDevNodesChanged = 0x0007; // DBT_DEVNODES_CHANGED
	if (nMsg == WM_DEVICECHANGE && wParam == kDevNodesChanged)
		InputManager::instance().on_device_changed();

	return wxFrame::MSWWindowProc(nMsg, wParam, lParam);
}
#endif

// ---- input on the canvas -------------------------------------------------------------------------

void MainWindow::OnMouseMove(wxMouseEvent& event)
{
	event.Skip();

	m_last_mouse_move_time = std::chrono::steady_clock::now();
	m_mouse_position = wxGetMousePosition();
	ShowCursor(true);

	auto& input = InputManager::instance();
	{
		std::unique_lock guard(input.m_main_mouse.m_mutex);
		const wxPoint phys = ToPhys(event.GetPosition());
		input.m_main_mouse.position = {phys.x, phys.y};
	}

	// fullscreen: the menu bar shows while the pointer is near the top edge
	if (IsFullScreen() && GetWxGUIConfig().fullscreen_menubar)
		SetMenuVisible(event.GetPosition().y < 50);
}

void MainWindow::OnMouseLeft(wxMouseEvent& event)
{
	auto& input = InputManager::instance();
	std::scoped_lock guard(input.m_main_mouse.m_mutex);
	const bool down = event.ButtonDown(wxMOUSE_BTN_LEFT);
	const wxPoint phys = ToPhys(event.GetPosition());
	input.m_main_mouse.left_down = down;
	input.m_main_mouse.position = {phys.x, phys.y};
	if (down)
		input.m_main_mouse.left_down_toggle = true;

	// docked into RetroPixels, clicking the game is how it gets the keyboard back
	if (RPEmbed::Enabled() && event.ButtonDown() && m_render_canvas && !m_render_canvas->HasFocus())
		m_render_canvas->SetFocus();

	event.Skip();
}

void MainWindow::OnMouseRight(wxMouseEvent& event)
{
	auto& input = InputManager::instance();
	std::scoped_lock guard(input.m_main_mouse.m_mutex);
	const bool down = event.ButtonDown(wxMOUSE_BTN_RIGHT);
	const wxPoint phys = ToPhys(event.GetPosition());
	input.m_main_mouse.right_down = down;
	input.m_main_mouse.position = {phys.x, phys.y};
	if (down)
		input.m_main_mouse.right_down_toggle = true;

	event.Skip();
}

void MainWindow::OnMouseWheel(wxMouseEvent& event)
{
	// one notch is 120
	InputManager::instance().m_mouse_wheel = (float)event.GetWheelRotation() / 120.0f;
	event.Skip();
}

void MainWindow::OnGesturePan(wxPanGestureEvent& event)
{
	auto& input = InputManager::instance();
	std::scoped_lock guard(input.m_main_touch.m_mutex);
	const wxPoint phys = ToPhys(event.GetPosition());
	const bool touching = event.IsGestureStart() || !event.IsGestureEnd();
	input.m_main_touch.position = {phys.x, phys.y};
	input.m_main_touch.left_down = touching;
	if (touching)
		input.m_main_touch.left_down_toggle = true;

	event.Skip();
}

void MainWindow::OnKeyUp(wxKeyEvent& event)
{
	event.Skip();
	if (!swkbd_hasKeyboardInputHook())
		HotkeySettings::CaptureInput(event);
}

void MainWindow::OnKeyDown(wxKeyEvent& event)
{
	// the canvas eats keys, so the usual quit shortcut is handled here
#if defined(__APPLE__)
	const bool quit = event.CmdDown() && event.GetKeyCode() == 'Q';
#else
	const bool quit = event.AltDown() && event.GetKeyCode() == WXK_F4;
#endif
	if (quit)
		Close(true);
	else
		event.Skip();
}

void MainWindow::OnChar(wxKeyEvent& event)
{
	// typed characters go to the software keyboard when a game has it open; not passed on
	if (swkbd_hasKeyboardInputHook())
		swkbd_keyInput(event.GetUnicodeKey());
}

void MainWindow::ShowCursor(bool state)
{
#if BOOST_OS_WINDOWS
	// ::ShowCursor keeps a display counter; move it until it is on the wanted side of zero
	CURSORINFO info{};
	info.cbSize = sizeof(info);
	GetCursorInfo(&info);
	if (state == (info.flags == CURSOR_SHOWING))
		return;
	if (state)
		while (::ShowCursor(TRUE) < 0) {}
	else
		while (::ShowCursor(FALSE) >= 0) {}
#else
	wxSetCursor(state ? wxNullCursor : wxCursor(wxCURSOR_BLANK));
#endif
}

void MainWindow::AsyncSetTitle(std::string_view windowTitle)
{
	auto* request = new wxCommandEvent(wxEVT_SET_WINDOW_TITLE);
	request->SetString(wxString::FromUTF8(windowTitle));
	g_mainFrame->QueueEvent(request);
}

void MainWindow::OnSetWindowTitle(wxCommandEvent& event)
{
	SetTitle(event.GetString());
}

// ---- fullscreen and menu bar ---------------------------------------------------------------------

void MainWindow::SetFullScreen(bool state)
{
	if (RPEmbed::Enabled())
		return; // RetroPixels' window does fullscreen
	if (!LaunchSettings::FullscreenEnabled().has_value()) // -f overrides without being saved
	{
		GetWxGUIConfig().fullscreen = state;
		g_wxConfig.Save();
	}
	if (state && !m_game_launched)
		return;

	g_window_info.is_fullscreen = state;
	m_fullscreenMenuItem->Check(state);
	ShowFullScreen(state);

	if (state)
		m_menu_visible = false; // wx takes the menu bar away in fullscreen
	else
		SetMenuVisible(true);
}

void MainWindow::SetMenuVisible(bool state)
{
	if (RPEmbed::Enabled() || state == m_menu_visible)
		return;
#if !BOOST_OS_MACOS // detaching the menu bar misbehaves on macOS
	SetMenuBar(state ? m_menuBar : nullptr);
#endif
	m_menu_visible = state;
}

bool MainWindow::IsMenuHidden() const
{
	// note: reports whether the menu bar is *shown* (callers rely on this)
	return m_menu_visible;
}

// ---- settings ------------------------------------------------------------------------------------

void MainWindow::LoadSettings()
{
	GetConfigHandle().Load();
	const auto& config = GetWxGUIConfig();

	// (update checks are turned off in PixelU)

	// docked: RetroPixels decides where the window is and how big
	const bool docked = RPEmbed::Enabled();
	if (!docked && config.window_position != Vector2i{-1, -1})
		SetPosition({config.window_position.x, config.window_position.y});
	if (!docked && config.window_size.x > 0 && config.window_size.y > 0)
	{
		SetSize({config.window_size.x, config.window_size.y});
		if (config.window_maximized)
			Maximize();
	}

	if (config.pad_position != Vector2i{-1, -1})
	{
		g_window_info.restored_pad_x = config.pad_position.x;
		g_window_info.restored_pad_y = config.pad_position.y;
	}
	if (config.pad_size != Vector2i{-1, -1})
	{
		g_window_info.restored_pad_width = config.pad_size.x;
		g_window_info.restored_pad_height = config.pad_size.y;
		g_window_info.pad_maximized = config.pad_maximized;
	}

	TogglePadView();

	if (m_game_list)
		m_game_list->LoadConfig();
}

void MainWindow::SaveSettings()
{
	if (RPEmbed::Enabled())
		return; // the docked layout is not the user's own; keep the saved one

	auto configLock = GetConfigHandle().Lock();
	auto& config = GetWxGUIConfig();

	// a stored -1,-1 means "not remembered" and stays that way
	if (config.window_position != Vector2i{-1, -1})
		config.window_position = {m_restored_position.x, m_restored_position.y};
	if (config.window_size != Vector2i{-1, -1})
	{
		config.window_size = {m_restored_size.x, m_restored_size.y};
		config.window_maximized = IsMaximized();
	}
	else
		config.window_maximized = false;

	config.pad_open = m_padView != nullptr;
	if (config.pad_position != Vector2i{-1, -1} && g_window_info.restored_pad_x != -1)
		config.pad_position = {g_window_info.restored_pad_x, g_window_info.restored_pad_y};
	if (config.pad_size != Vector2i{-1, -1} && g_window_info.restored_pad_width != -1)
	{
		config.pad_size = {g_window_info.restored_pad_width, g_window_info.restored_pad_height};
		config.pad_maximized = g_window_info.pad_maximized;
	}
	else
		config.pad_maximized = false;

	if (m_game_list)
		m_game_list->SaveConfig();

	g_wxConfig.Save();
}

void MainWindow::OpenSettings()
{
	auto& config = GetWxGUIConfig();
	const auto uiLanguageBefore = config.language;

	GeneralSettings2 dialog(this, m_game_launched);
	dialog.ShowModal();
	const bool gamePathsChanged = dialog.ShouldReloadGamelist();
	const bool mlcChanged = dialog.MLCModified();

	if (gamePathsChanged)
	{
		if (m_game_list)
			m_game_list->ReloadGameEntries();
	}
	else
		SaveSettings();

#ifdef ENABLE_DISCORD_RPC
	if (!config.use_discord_presence)
		m_discord.reset();
	else if (!m_discord)
	{
		m_discord = std::make_unique<DiscordPresence>();
		if (!m_launched_game_name.empty())
			m_discord->UpdatePresence(DiscordPresence::Playing, m_launched_game_name);
	}
#endif

	// (update checks are turned off in PixelU)

	if (mlcChanged)
		RecreateMenu();
	if (!config.fullscreen_menubar && IsFullScreen())
		SetMenuVisible(false);

	if (config.language != uiLanguageBefore)
		wxMessageBox(_("PixelU must be restarted to apply the selected UI language."), _("Information"), wxOK | wxCENTRE, this);
}

// ---- GamePad window, tool windows ----------------------------------------------------------------

void MainWindow::TogglePadView()
{
	// docked into RetroPixels there is never a separate GamePad window
	const bool wanted = GetWxGUIConfig().pad_open && !RPEmbed::Enabled();
	if (!wanted)
	{
		if (m_padView)
		{
			m_padView->Destroy();
			m_padView = nullptr;
		}
		return;
	}
	if (m_padView)
		return;

	m_padView = new PadViewFrame(this);
	m_padView->Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnPadClose, this);
	m_padView->Show(true);
#if (BOOST_OS_LINUX || BOOST_OS_BSD) && HAS_WAYLAND
	if (wxWlIsWaylandWindow(m_padView))
		wxWlSetAppId(m_padView, "io.github.shredder565.PixelU");
#endif
	m_padView->Initialize();
	if (m_game_launched)
		m_padView->InitializeRenderCanvas();
}

void MainWindow::OnPadClose(wxCloseEvent& event)
{
	auto& config = GetWxGUIConfig();
	config.pad_open = false;
	if (config.pad_position != Vector2i{-1, -1})
		m_padView->GetPosition(&config.pad_position.x, &config.pad_position.y);
	if (config.pad_size != Vector2i{-1, -1})
		m_padView->GetSize(&config.pad_size.x, &config.pad_size.y);
	g_wxConfig.Save();

	m_padView = nullptr; // wx destroys the frame itself
	if (m_padViewMenuItem)
		m_padViewMenuItem->Check(false);

	event.Skip();
}

void MainWindow::OnGraphicWindowOpen(wxTitleIdEvent& event)
{
	if (m_graphic_pack_window)
		return;
	m_graphic_pack_window = new GraphicPacksWindow2(this, event.GetTitleId());
	m_graphic_pack_window->Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnGraphicWindowClose, this);
	m_graphic_pack_window->Show(true);
}

void MainWindow::OnGraphicWindowClose(wxCloseEvent&)
{
	m_graphic_pack_window->Destroy();
	m_graphic_pack_window = nullptr;
}

void MainWindow::OnMemorySearcherClose(wxCloseEvent& event)
{
	m_toolWindow = nullptr;
	event.Skip();
}

void MainWindow::OnDebuggerClose(wxCloseEvent& event)
{
	m_debugger_window = nullptr;
	event.Skip();
}

wxRect MainWindow::GetDesktopRect()
{
	// the display under the window's horizontal centre
	const int centreX = GetPosition().x + GetSize().GetWidth() / 2;
	for (unsigned i = 0; i < wxDisplay::GetCount(); ++i)
	{
		wxDisplay display(i);
		if (!display.IsOk())
			continue;
		const wxRect area = display.GetGeometry();
		if (centreX >= area.x && centreX <= area.x + area.width)
			return area;
	}
	return {0, 0, 800, 600};
}

void MainWindow::OpenPPCDebugger()
{
	if (m_debugger_window)
		return;

	// the main window shrinks and moves down; the debugger takes the space around it
	const wxRect desktop = GetDesktopRect();
	SetSize(800, 500);
	CenterOnScreen();
	wxPoint position = GetPosition();
	position.y = std::min(position.y + 200, desktop.GetHeight() - 400);
	SetPosition(position);

	m_debugger_window = new DebuggerWindow2(*this, desktop);
	m_debugger_window->Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnDebuggerClose, this);
	m_debugger_window->Show(true);
}

// ---- File menu -----------------------------------------------------------------------------------

void MainWindow::OnFileMenu(wxCommandEvent& event)
{
	const int id = event.GetId();
	if (id == Cmd::Load)
	{
		// wx filter syntax: label|patterns|label|patterns...
		struct FileKind
		{
			wxString label;
			const char* patterns;
		};
		const FileKind kinds[] = {
			{_("All Wii U files (*.wud, *.wux, *.wua, *.wuhb, *.iso, *.rpx, *.elf)"), "*.wud;*.wux;*.wua;*.wuhb;*.iso;*.rpx;*.elf;title.tmd"},
			{_("Wii U image (*.wud, *.wux, *.iso, *.wad)"), "*.wud;*.wux;*.iso"},
			{_("Wii U NUS content"), "title.tmd"},
			{_("Wii U archive (*.wua)"), "*.wua"},
			{_("Wii U homebrew bundle (*.wuhb)"), "*.wuhb"},
			{_("Wii U executable (*.rpx, *.elf)"), "*.rpx;*.elf"},
			{_("All files (*.*)"), "*"},
		};
		wxString filter;
		for (const FileKind& kind : kinds)
		{
			if (!filter.empty())
				filter << '|';
			filter << kind.label << '|' << wxString::FromUTF8(kind.patterns);
		}
		wxFileDialog picker(this, _("Open file to launch"), wxEmptyString, wxEmptyString, filter, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (picker.ShowModal() == wxID_CANCEL || picker.GetPath().IsEmpty())
			return;
		FileLoad(_utf8ToPath(picker.GetPath().utf8_string()), wxLaunchGameEvent::INITIATED_BY::MENU);
	}
	else if (id >= Cmd::RecentFirst && id <= Cmd::RecentLast)
	{
		const auto& recent = GetWxGUIConfig().recent_launch_files;
		const size_t slot = (size_t)(id - Cmd::RecentFirst);
		if (slot >= recent.size())
			return;
		const fs::path path = _utf8ToPath(recent[slot]);
		if (!path.empty())
			FileLoad(path, wxLaunchGameEvent::INITIATED_BY::MENU);
	}
	else if (id == Cmd::CloseGame)
	{
		EndEmulation();
	}
}

void MainWindow::OnOpenFolder(wxCommandEvent& event)
{
	fs::path folder;
	switch (event.GetId())
	{
	case Cmd::OpenUserFolder: folder = ActiveSettings::GetUserDataPath(); break;
	case Cmd::OpenMlcFolder: folder = ActiveSettings::GetMlcPath(); break;
	case Cmd::OpenShaderCacheFolder: folder = ActiveSettings::GetCachePath("shaderCache"); break;
	default: return;
	}
	wxLaunchDefaultApplication(wxHelper::FromPath(folder));
}

void MainWindow::OnClearSpotPassCache(wxCommandEvent&)
{
	const fs::path boss = ActiveSettings::GetMlcPath("usr/boss");
	fs::remove_all(boss);
	fs::create_directory(boss); // leave an empty usr/boss behind
	wxMessageBox(_("SpotPass cache cleared"));
}

void MainWindow::OnInstallUpdate(wxCommandEvent&)
{
	for (;;)
	{
		wxDirDialog picker(nullptr, _("Select folder of title to install"), "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST,
						   wxDefaultPosition, wxDefaultSize, _("Select the folder that stores your update, DLC or base game files"));
		const int choice = picker.ShowModal();
		if (choice == wxID_CANCEL || picker.GetPath().IsEmpty())
			return;
		if (choice != wxID_OK)
			continue;

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
		fs::path folder((const char*)picker.GetPath().fn_str());
#else
		fs::path folder(picker.GetPath().fn_str());
#endif
		// picking code/, content/ or meta/ means the title folder above it
		const fs::path leaf = folder.filename();
		if ((leaf == "code" || leaf == "content" || leaf == "meta") && folder.has_parent_path())
		{
			const fs::path parent = folder.parent_path();
			const bool complete = fs::exists(parent / "code") && fs::exists(parent / "content") && fs::exists(parent / "meta");
			if (!complete)
			{
				wxMessageBox(formatWxString(_("The (parent) folder of the title you selected is missing at least one of the required subfolders (\"code\", \"content\" and \"meta\")\nMake sure that the files are complete."), leaf.string()));
				continue;
			}
			folder = parent;
		}

		if (!fs::exists(folder))
			wxMessageBox(_("The folder you have selected cannot be found on your system."));
		else if (!fs::exists(folder / "meta" / "meta.xml"))
			wxMessageBox(_("Unable to find the /meta/meta.xml file inside the selected folder."));
		else
		{
			InstallUpdate(folder);
			return;
		}
	}
}

bool MainWindow::InstallUpdate(const fs::path& metaFilePath)
{
	try
	{
		GameUpdateWindow installer(*this, metaFilePath);
		if (installer.ShowModal() == wxID_OK)
		{
			// also refreshes the game list entry
			CafeTitleList::AddTitleFromPath(installer.GetTargetPath());
			wxMessageBox(_("Title installed!"), _("Success"));
			return true;
		}
		if (!installer.GetExceptionMessage().empty())
			throw std::runtime_error(installer.GetExceptionMessage());
		wxMessageBox(_("Title installation has been canceled!"));
	}
	catch (const AbortException&)
	{
	}
	catch (const std::exception& error)
	{
		wxMessageBox(error.what(), _("Update error"));
	}
	return false;
}

void MainWindow::OnFileExit(wxCommandEvent&)
{
	Close();
}

// ---- NFC menu ------------------------------------------------------------------------------------

void MainWindow::OnNFCMenu(wxCommandEvent& event)
{
	const int id = event.GetId();
	std::string file;
	if (id == Cmd::TouchNfcFile)
	{
		wxFileDialog picker(this, _("Open file to load"), "", "", "All NFC files (bin, dat, nfc)|*.bin;*.dat;*.nfc|All files (*.*)|*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (picker.ShowModal() == wxID_CANCEL || picker.GetPath().IsEmpty())
			return;
		file = picker.GetPath().utf8_string();
	}
	else if (id >= Cmd::NfcRecentFirst && id <= Cmd::NfcRecentLast)
	{
		const auto& recent = GetWxGUIConfig().recent_nfc_files;
		const size_t slot = (size_t)(id - Cmd::NfcRecentFirst);
		if (slot >= recent.size() || recent[slot].empty())
			return;
		file = recent[slot];
	}
	else
		return;

	uint32 nfcError = 0;
	if (nfc::TouchTagFromFile(_utf8ToPath(file), &nfcError))
	{
		GetWxGUIConfig().AddRecentNfcFile(file);
		UpdateNFCMenu();
		return;
	}
	const wxString text = NfcErrorText(nfcError);
	if (!text.empty())
		wxMessageBox(text);
}

void MainWindow::UpdateNFCMenu()
{
	if (m_nfcMenuSeparator0)
	{
		m_nfcMenu->Remove(m_nfcMenuSeparator0->GetId());
		m_nfcMenuSeparator0 = nullptr;
	}
	for (int slot = 0; slot < (int)wxCemuConfig::kMaxRecentEntries; ++slot)
	{
		if (m_nfcMenu->FindChildItem(Cmd::NfcRecentFirst + slot))
			m_nfcMenu->Remove(Cmd::NfcRecentFirst + slot);
	}

	// existing files only, at most 12, numbered from 0; an item's id keeps its slot in the list
	const auto& recent = GetWxGUIConfig().recent_nfc_files;
	int shown = 0;
	for (size_t slot = 0; slot < recent.size() && shown < 12; ++slot)
	{
		const std::string& file = recent[slot];
		if (file.empty() || !fs::exists(_utf8ToPath(file)))
			continue;
		if (shown == 0)
			m_nfcMenuSeparator0 = m_nfcMenu->AppendSeparator();
		m_nfcMenu->Append(Cmd::NfcRecentFirst + (int)slot, formatWxString("{}. {}", shown, file));
		++shown;
	}
}

// ---- Options menu --------------------------------------------------------------------------------

void MainWindow::OnOptionsInput(wxCommandEvent& event)
{
	switch (event.GetId())
	{
	case Cmd::Fullscreen:
		SetFullScreen(m_fullscreenMenuItem->IsChecked());
		break;
	case Cmd::GamePadWindow:
	{
		auto& config = GetWxGUIConfig();
		config.pad_open = !config.pad_open;
		g_wxConfig.Save();
		TogglePadView();
		break;
	}
	case Cmd::GraphicPacks:
	{
		if (m_graphic_pack_window)
			return;
		const uint64 titleId = CafeSystem::IsTitleRunning() ? CafeSystem::GetForegroundTitleId() : 0;
		m_graphic_pack_window = new GraphicPacksWindow2(this, titleId);
		m_graphic_pack_window->Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnGraphicWindowClose, this);
		m_graphic_pack_window->Show(true);
		break;
	}
	case Cmd::GeneralSettings:
	case Cmd::MacPreferences:
		OpenSettings();
		break;
	case Cmd::InputSettings:
	{
		auto* dialog = new InputSettings2(this);
		dialog->ShowModal();
		dialog->Destroy();
		break;
	}
	case Cmd::HotkeySettings:
		(new HotkeySettings(this))->Show();
		break;
	default:
		break;
	}
}

void MainWindow::OnAccountSelect(wxCommandEvent& event)
{
	const int slot = event.GetId() - Cmd::AccountFirst;
	const auto& accounts = Account::GetAccounts();
	wxASSERT(slot >= 0 && slot < (int)accounts.size());
	GetConfig().account.m_persistent_id = accounts[slot].GetPersistentId();
	GetConfigHandle().Save();
}

void MainWindow::OnConsoleLanguage(wxCommandEvent& event)
{
	const auto match = std::find_if(std::begin(kConsoleLanguages), std::end(kConsoleLanguages),
									[&](const LanguageEntry& entry) { return entry.cmd == event.GetId(); });
	if (match != std::end(kConsoleLanguages))
		GetConfig().console_language = match->language;
	else
		cemu_assert_debug(false);

	// titles are shown in the console language
	if (m_game_list)
	{
		m_game_list->DeleteCachedStrings();
		m_game_list->ReloadGameEntries();
	}
	GetConfigHandle().Save();
}

// ---- Tools menu ----------------------------------------------------------------------------------

void MainWindow::OnToolsInput(wxCommandEvent& event)
{
	const int id = event.GetId();
	if (id == Cmd::MemorySearcher)
	{
		if (m_toolWindow)
		{
			m_toolWindow->SetFocus();
			return;
		}
		m_toolWindow = new MemorySearcherTool(this);
		m_toolWindow->Bind(wxEVT_CLOSE_WINDOW, &MainWindow::OnMemorySearcherClose, this);
		m_toolWindow->Show(true);
	}
	else if (id == Cmd::TitleManagerTab || id == Cmd::DownloadManagerTab)
	{
		const auto page = id == Cmd::TitleManagerTab ? TitleManagerPage::TitleManager : TitleManagerPage::DownloadManager;
		if (m_title_manager)
		{
			m_title_manager->SetFocusAndTab(page);
			return;
		}
		m_title_manager = new TitleManager(this, page);
		m_title_manager->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& close) {
			m_title_manager = nullptr;
			close.Skip();
		});
		m_title_manager->Show();
	}
	else if (id == Cmd::UsbDevices)
	{
		if (m_usb_devices)
		{
			m_usb_devices->Show(true);
			m_usb_devices->Raise();
			m_usb_devices->SetFocus();
			return;
		}
		// the device window only hides when closed; its state lives on
		m_usb_devices = new EmulatedUSBDeviceFrame(this);
		m_usb_devices->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& close) {
			if (close.CanVeto())
			{
				m_usb_devices->Show(false);
				close.Veto();
			}
		});
		m_usb_devices->Show(true);
	}
}

// ---- CPU and Debug menus -------------------------------------------------------------------------

void MainWindow::OnDebugSetting(wxCommandEvent& event)
{
	const int id = event.GetId();
	auto& config = GetConfig();

	if (id >= Cmd::TimerSpeedFirst && id <= Cmd::TimerSpeedLast)
		ActiveSettings::SetTimerShiftFactor(kTimerShiftForCmd[id - Cmd::TimerSpeedFirst]);
	else if (id == Cmd::RenderUpsideDown)
		config.render_upside_down = event.IsChecked();
	else if (id == Cmd::AccurateBarriers)
	{
		config.vk_accurate_barriers = event.IsChecked();
		if (!config.vk_accurate_barriers)
			wxMessageBox(_("Warning: Disabling the accurate barriers option will lead to flickering graphics but may improve performance. It is highly recommended to leave it turned on."), _("Accurate barriers are off"), wxOK);
	}
	else if (id == Cmd::AudioAuxOnly)
		ActiveSettings::EnableAudioOnlyAux(event.IsChecked());
	else if (id == Cmd::DumpRam)
		memory_createDump();
	else if (id == Cmd::DumpRpx)
		CafeSystem::DumpCurrentRPX();
	else if (id == Cmd::DumpFst)
	{
		// no menu item for this any more
	}
	else if (id == Cmd::DumpCurl)
	{
		const bool enable = event.IsChecked();
		ActiveSettings::EnableDumpLibcurlRequests(enable);
		if (enable)
		{
			try
			{
				fs::create_directories(fs::path(ActiveSettings::GetUserDataPath()) / "dump" / "curl");
			}
			catch (const std::exception& error)
			{
				SystemException detail(error);
				cemuLog_log(LogType::Force, "error when creating dump curl folder: {}", detail.what());
				ActiveSettings::EnableDumpLibcurlRequests(false);
			}
		}
	}
	else
		cemu_assert_debug(false);

	GetConfigHandle().Save();
}

void MainWindow::OnDebugLoggingToggleFlagGeneric(wxCommandEvent& event)
{
	const int offset = event.GetId() - Cmd::LogFirst;
	if (offset < 0 || offset >= 64)
		return;
	const auto flag = cemuLog_getFlag(static_cast<LogType>(offset));
	auto& config = GetConfig();
	const auto current = config.log_flag.GetValue();
	config.log_flag = event.IsChecked() ? (current | flag) : (current & ~flag);
	cemuLog_setActiveLoggingFlags(config.log_flag.GetValue());
	GetConfigHandle().Save();
}

void MainWindow::OnPPCInfoToggle(wxCommandEvent&)
{
	auto& config = GetConfig();
	config.advanced_ppc_logging = !config.advanced_ppc_logging.GetValue();
	GetConfigHandle().Save();
}

void MainWindow::OnDebugDumpGeneric(wxCommandEvent& event)
{
	struct DumpKind
	{
		int cmd;
		const char* folder;
		void (*enable)(bool);
	};
	static const DumpKind kinds[] = {
		{Cmd::DumpTextures, "dump/textures", &ActiveSettings::EnableDumpTextures},
		{Cmd::DumpShaders, "dump/shaders", &ActiveSettings::EnableDumpShaders},
		{Cmd::DumpRecompiler, "dump/recompiler", &ActiveSettings::EnableDumpRecompilerFunctions},
	};
	const DumpKind* kind = nullptr;
	for (const DumpKind& candidate : kinds)
		if (candidate.cmd == event.GetId())
			kind = &candidate;
	if (!kind)
		UNREACHABLE;

	const bool enable = event.IsChecked();
	kind->enable(enable);
	if (!enable)
		return;
	try
	{
		fs::create_directories(ActiveSettings::GetUserDataPath(kind->folder));
	}
	catch (const std::exception& error)
	{
		cemuLog_log(LogType::Force, "can't create folder {} in user data folder: {}", kind->folder, error.what());
		kind->enable(false);
	}
}

void MainWindow::OnLoggingWindow(wxCommandEvent&)
{
	if (m_logging_window)
		return;
	m_logging_window = new LoggingWindow(this);
	m_logging_window->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& close) {
		m_logging_window = nullptr;
		close.Skip();
	});
	m_logging_window->Show(true);
}

void MainWindow::OnGDBStubToggle(wxCommandEvent&)
{
	if (g_gdbstub)
	{
		// let go of the running server without tearing it down
		(void)g_gdbstub.release();
		return;
	}
	g_gdbstub = std::make_unique<GDBServer>(GetConfig().gdb_port);
}

void MainWindow::OnDebugViewPPCThreads(wxCommandEvent&)
{
	(new DebugPPCThreadsWindow(*this))->Show(true);
}

void MainWindow::OnDebugViewPPCDebugger(wxCommandEvent&)
{
	if (m_debugger_window && m_debugger_window->IsShown())
	{
		m_debugger_window->Close();
		m_debugger_window = nullptr;
		return;
	}
	OpenPPCDebugger();
}

void MainWindow::OnDebugViewAudioDebugger(wxCommandEvent&)
{
	(new AudioDebuggerWindow(*this))->Show(true);
}

void MainWindow::OnDebugViewTextureRelations(wxCommandEvent&)
{
	openTextureViewer(*this);
}

// ---- Help menu -----------------------------------------------------------------------------------

namespace
{
	class AboutDialog : public wxDialog
	{
	public:
		explicit AboutDialog(wxWindow* parent)
			: wxDialog(nullptr, wxID_ANY, _("About PixelU"), wxDefaultPosition, wxSize(500, 700))
		{
			SetIcon(wxICON(M_WND_ICON128));

			auto* page = new wxScrolledWindow(this);
			auto* content = new wxBoxSizer(wxVERTICAL);
			AddVersionInfo(page, content);
			content->AddSpacer(5);
			AddLibraries(page, content);
			content->AddSpacer(5);
			AddThanks(page, content);
			page->SetSizer(content);
			page->FitInside();
			page->SetScrollRate(25, 25);

			auto* outer = new wxBoxSizer(wxVERTICAL);
			outer->Add(page, wxSizerFlags(1).Expand().Border(wxLEFT, 10));
			SetSizer(outer);
			CentreOnParent();
		}

	private:
		static constexpr long kLinkStyle = wxHL_CONTEXTMENU | wxNO_BORDER | wxHL_ALIGN_LEFT;

		static wxHyperlinkCtrl* Link(wxWindow* parent, const wxString& label, const wxString& url)
		{
			return new wxHyperlinkCtrl(parent, wxID_ANY, label, url, wxDefaultPosition, wxDefaultSize, kLinkStyle);
		}

		static void AddRule(wxWindow* parent, wxSizer* sizer)
		{
			sizer->AddSpacer(3);
			sizer->Add(new wxStaticLine(parent), wxSizerFlags().Expand().Border(wxRIGHT, 4));
		}

		static void AddVersionInfo(wxWindow* parent, wxSizer* sizer)
		{
			const wxString version = formatWxString(_("PixelU (based on Cemu)\nVersion {0}\nCompiled on {1}\nCemu's original authors: {2}"),
													BUILD_VERSION_STRING, __DATE__ " " __TIME__, "Exzap, Petergov");
			sizer->Add(new wxStaticText(parent, wxID_ANY, version), wxSizerFlags().Border(wxALL, 3).Border(wxTOP, 10));
			sizer->Add(Link(parent, "https://cemu.info", "https://cemu.info"), wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, 3));
			AddRule(parent, sizer);
			sizer->AddSpacer(5);
			sizer->Add(new wxStaticText(parent, wxID_ANY, _("PixelU is a Wii U emulator for RetroPixels, built from Cemu's source\nunder the Mozilla Public License 2.0.\n\nWii and Wii U are trademarks of Nintendo.\nPixelU and Cemu are not affiliated with Nintendo.")));
		}

		// "name (link)" on one line
		static void AddLibrary(wxWindow* parent, wxSizer* sizer, const char* name, const char* url)
		{
			auto* line = new wxBoxSizer(wxHORIZONTAL);
			line->Add(new wxStaticText(parent, wxID_ANY, wxString::FromUTF8(name) + " ("));
			line->Add(Link(parent, url, url));
			line->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(line);
		}

		static void AddLibraries(wxWindow* parent, wxSizer* sizer)
		{
			AddRule(parent, sizer);
			sizer->AddSpacer(3);
			sizer->Add(new wxStaticText(parent, wxID_ANY, _("Used libraries and utilities:")), wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, 2));

			static const std::pair<const char*, const char*> libraries[] = {
				{"zLib", "https://www.zlib.net"},
				{"wxWidgets", "https://www.wxwidgets.org/"},
				{"OpenSSL", "https://www.openssl.org/"},
				{"libcurl", "https://curl.haxx.se/libcurl/"},
				{"imgui", "https://github.com/ocornut/imgui"},
				{"fontawesome", "https://github.com/FortAwesome/Font-Awesome"},
				{"boost", "https://www.boost.org"},
				{"libusb", "https://libusb.info"},
			};
			for (const auto& [name, url] : libraries)
				AddLibrary(parent, sizer, name, url);
#if BOOST_OS_MACOS
			AddLibrary(parent, sizer, "MoltenVK", "https://github.com/KhronosGroup/MoltenVK");
#endif
			{
				auto* line = new wxBoxSizer(wxHORIZONTAL);
				line->Add(new wxStaticText(parent, wxID_ANY, "icons from "));
				line->Add(Link(parent, "https://icons8.com", "https://icons8.com"));
				sizer->Add(line);
			}
			sizer->Add(new wxStaticText(parent, wxID_ANY, "\"Lato\" font by tyPoland Lukasz Dziedzic (OFL, V1.1)"));
			AddLibrary(parent, sizer, "SDL", "https://github.com/libsdl-org/SDL");

			// ih264: source download plus its licence notice (written to a temp file and opened)
			auto* line = new wxBoxSizer(wxHORIZONTAL);
			line->Add(new wxStaticText(parent, wxID_ANY, "Modified ih264 from Android project ("));
			line->Add(Link(parent, "Source", "https://cemu.info/oss/ih264d.zip"));
			line->Add(new wxStaticText(parent, wxID_ANY, " "));
			auto* notice = Link(parent, "NOTICE", "");
			notice->Bind(wxEVT_LEFT_DOWN, [](wxMouseEvent&) { OpenIh264Notice(); });
			line->Add(notice);
			line->Add(new wxStaticText(parent, wxID_ANY, ")"));
			sizer->Add(line);
		}

		static void OpenIh264Notice()
		{
			const fs::path file = fs::temp_directory_path() / "NOTICE_IH264.txt";
			std::unique_ptr<FileStream> out(FileStream::createFile2(file));
			if (!out)
				return;
			out->writeString(
				"/******************************************************************************\r\n"
				" *\r\n"
				" * Copyright (C) 2015 The Android Open Source Project\r\n"
				" *\r\n"
				" * Licensed under the Apache License, Version 2.0 (the \"License\");\r\n"
				" * you may not use this file except in compliance with the License.\r\n"
				" * You may obtain a copy of the License at:"
				" *\r\n"
				" * http://www.apache.org/licenses/LICENSE-2.0\r\n"
				" *\r\n"
				" * Unless required by applicable law or agreed to in writing, software\r\n"
				" * distributed under the License is distributed on an \"AS IS\" BASIS,\r\n"
				" * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\r\n"
				" * See the License for the specific language governing permissions and\r\n"
				" * limitations under the License.\r\n"
				" *\r\n"
				" *****************************************************************************\r\n"
				" * Originally developed and contributed by Ittiam Systems Pvt. Ltd, Bangalore\r\n"
				"*/\r\n"
				"/*****************************************************************************/\r\n");
			out.reset(); // flush and close before the browser opens it
			wxLaunchDefaultBrowser(formatWxString("file:{}", _pathToUtf8(file)));
		}

		static void AddThanks(wxWindow* parent, wxSizer* sizer)
		{
			AddRule(parent, sizer);
			sizer->AddSpacer(5);
			sizer->Add(new wxStaticText(parent, wxID_ANY, _("Thanks to the Cemu team and everyone who contributed to Cemu, whose work PixelU builds on.")),
					   wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, 2));
		}
	};
}

void MainWindow::OnHelpAbout(wxCommandEvent&)
{
	AboutDialog about(this);
	about.ShowModal();
}

void MainWindow::OnHelpUpdate(wxCommandEvent&)
{
	CemuUpdateWindow updater(this);
	updater.ShowModal();
}

// ---- the menu bar --------------------------------------------------------------------------------

void MainWindow::RecreateMenu()
{
	if (m_menuBar)
	{
		SetMenuBar(nullptr);
		m_menuBar->Destroy();
		m_menuBar = nullptr;
	}

	auto& guiConfig = GetWxGUIConfig();
	auto& config = GetConfig();
	m_menuBar = new wxMenuBar();

	// File
	m_fileMenu = new wxMenu();
	m_fileMenuSeparator0 = nullptr;
	m_fileMenuSeparator1 = nullptr;
	if (!m_game_launched)
	{
		m_loadMenuItem = m_fileMenu->Append(Cmd::Load, _("&Load..."));
		m_installUpdateMenuItem = m_fileMenu->Append(Cmd::InstallTitle, _("&Install game title, update or DLC..."));

		// numbered from 1, at most 9; an item's id keeps its slot in the list
		auto* recentMenu = new wxMenu();
		int number = 1;
		for (size_t slot = 0; slot < guiConfig.recent_launch_files.size() && number < 10; ++slot)
		{
			const std::string& file = guiConfig.recent_launch_files[slot];
			if (file.empty())
				continue;
			recentMenu->Append(Cmd::RecentFirst + (int)slot, formatWxString("{}. {}", number, file));
			++number;
		}
		m_fileMenu->AppendSeparator();
		m_fileMenu->AppendSubMenu(recentMenu, _("Recent files"));
		m_fileMenu->AppendSeparator();
	}
	else
	{
#ifdef CEMU_DEBUG_ASSERT
		m_fileMenu->Append(Cmd::CloseGame, _("Close game"));
		m_fileMenuSeparator1 = m_fileMenu->AppendSeparator();
#endif
	}
	m_fileMenu->Append(Cmd::OpenUserFolder, _("Open PixelU folder"));
	m_fileMenu->Append(Cmd::OpenMlcFolder, _("Open MLC folder"));
	m_fileMenu->Append(Cmd::OpenShaderCacheFolder, _("Open &shader cache folder"));
	m_fileMenu->AppendSeparator();
	m_fileMenu->Append(Cmd::ClearSpotPass, _("Clear Spot&Pass cache"))->Enable(!m_game_launched);
	m_fileMenu->AppendSeparator();
	m_exitMenuItem = m_fileMenu->Append(Cmd::Exit, _("&Exit"));
	m_menuBar->Append(m_fileMenu, _("&File"));

	// Options > Active account
	m_optionsAccountMenu = new wxMenu();
	{
		const auto activeId = ActiveSettings::GetPersistentId();
		const bool locked = m_game_launched || LaunchSettings::GetPersistentId().has_value();
		int slot = 0;
		for (const auto& account : Account::GetAccounts())
		{
			wxMenuItem* item = m_optionsAccountMenu->AppendRadioItem(Cmd::AccountFirst + slot, account.ToString());
			item->Check(account.GetPersistentId() == activeId);
			if (locked)
				item->Enable(false);
			++slot;
		}
	}

	// Options > Console language (fixed while a game runs)
	auto* languageMenu = new wxMenu();
	for (const LanguageEntry& entry : kConsoleLanguages)
	{
		wxMenuItem* item = languageMenu->AppendRadioItem(entry.cmd, wxGetTranslation(entry.label));
		item->Check(config.console_language == entry.language);
	}
	if (IsGameLaunched())
	{
		for (wxMenuItem* item : languageMenu->GetMenuItems())
			item->Enable(false);
	}

	// Options
	auto* optionsMenu = new wxMenu();
	m_fullscreenMenuItem = optionsMenu->AppendCheckItem(Cmd::Fullscreen, _("&Fullscreen"));
	m_fullscreenMenuItem->Check(FullscreenEnabled());
	optionsMenu->Append(Cmd::GraphicPacks, _("&Graphic packs"));
	m_padViewMenuItem = optionsMenu->AppendCheckItem(Cmd::GamePadWindow, _("&Separate GamePad view"));
	m_padViewMenuItem->Check(guiConfig.pad_open);
	optionsMenu->AppendSeparator();
#if BOOST_OS_MACOS
	optionsMenu->Append(Cmd::MacPreferences, _("&Settings..." "\tCtrl-,"));
#endif
	optionsMenu->Append(Cmd::GeneralSettings, _("&General settings"));
	optionsMenu->Append(Cmd::InputSettings, _("&Input settings"));
	optionsMenu->Append(Cmd::HotkeySettings, _("&Hotkey settings"));
	optionsMenu->AppendSeparator();
	optionsMenu->AppendSubMenu(m_optionsAccountMenu, _("&Active account"));
	optionsMenu->AppendSubMenu(languageMenu, _("&Console language"));
	m_menuBar->Append(optionsMenu, _("&Options"));

	// Tools
	auto* toolsMenu = new wxMenu();
	m_memorySearcherMenuItem = toolsMenu->Append(Cmd::MemorySearcher, _("&Memory searcher"));
	m_memorySearcherMenuItem->Enable(false);
	toolsMenu->Append(Cmd::TitleManagerTab, _("&Title Manager"));
	toolsMenu->Append(Cmd::DownloadManagerTab, _("&Download Manager"));
	toolsMenu->Append(Cmd::UsbDevices, _("&Emulated USB Devices"));
	m_menuBar->Append(toolsMenu, _("&Tools"));

	// CPU > Timer speed
	{
		const char* const speedLabels[] = {
			wxTRANSLATE("&1x speed"), wxTRANSLATE("&2x speed"), wxTRANSLATE("&4x speed"), wxTRANSLATE("&8x speed"),
			wxTRANSLATE("&0.5x speed"), wxTRANSLATE("&0.25x speed"), wxTRANSLATE("&0.125x speed")};
		auto* speedMenu = new wxMenu();
		const auto shift = ActiveSettings::GetTimerShiftFactor();
		for (int i = 0; i <= Cmd::TimerSpeedLast - Cmd::TimerSpeedFirst; ++i)
			speedMenu->AppendRadioItem(Cmd::TimerSpeedFirst + i, wxGetTranslation(speedLabels[i]))->Check(shift == kTimerShiftForCmd[i]);
		auto* cpuMenu = new wxMenu();
		cpuMenu->AppendSubMenu(speedMenu, _("&Timer speed"));
		m_menuBar->Append(cpuMenu, _("&CPU"));
	}

	// NFC (the recent entries are added by UpdateNFCMenu once a game runs)
	m_nfcMenu = new wxMenu();
	m_nfcMenu->Append(Cmd::TouchNfcFile, _("&Scan NFC tag/amiibo from file"))->Enable(false);
	m_menuBar->Append(m_nfcMenu, _("&NFC"));
	m_nfcMenuSeparator0 = nullptr;

	// Debug > Logging
	m_loggingSubmenu = new wxMenu();
	AppendLogItems(m_loggingSubmenu, kLogGeneral);
	m_loggingSubmenu->AppendSeparator();
	{
		auto* modulesMenu = new wxMenu();
		modulesMenu->AppendCheckItem(Cmd::LogExpertNote, _("&Options below are for experts. Leave off if unsure"))->Enable(false);
		modulesMenu->AppendSeparator();
		AppendLogItems(modulesMenu, kLogCafeModules);
		m_loggingSubmenu->AppendSubMenu(modulesMenu, _("&CafeOS modules logging"));
	}
	m_loggingSubmenu->AppendSeparator();
	AppendLogItems(m_loggingSubmenu, kLogGraphics);
	m_loggingSubmenu->AppendSeparator();
	AppendLogItems(m_loggingSubmenu, kLogRenderer);
	m_loggingSubmenu->AppendCheckItem(Cmd::LogPpcContext, _("&Log PPC context for API"))->Check(cemuLog_advancedPPCLoggingEnabled());

	// Debug > Dump
	auto* dumpMenu = new wxMenu();
	dumpMenu->AppendCheckItem(Cmd::DumpTextures, _("&Textures"))->Check(ActiveSettings::DumpTexturesEnabled());
	dumpMenu->AppendCheckItem(Cmd::DumpShaders, _("&Shaders"))->Check(ActiveSettings::DumpShadersEnabled());
	dumpMenu->AppendCheckItem(Cmd::DumpRecompiler, _("&Recompiled functions"))->Check(ActiveSettings::DumpRecompilerFunctionsEnabled());
	dumpMenu->AppendCheckItem(Cmd::DumpCurl, _("&nlibcurl HTTP/HTTPS requests"));

	// Debug
	m_debugMenu = new wxMenu();
	m_debugMenu->AppendSubMenu(m_loggingSubmenu, _("&Logging"));
	m_debugMenu->AppendSubMenu(dumpMenu, _("&Dump"));
	m_debugMenu->AppendSeparator();

	wxMenuItem* upsideDown = m_debugMenu->AppendCheckItem(Cmd::RenderUpsideDown, _("&Render upside-down"));
	upsideDown->Check(ActiveSettings::RenderUpsideDownEnabled());
	if (LaunchSettings::RenderUpsideDownEnabled().has_value())
		upsideDown->Enable(false);
	m_debugMenu->AppendCheckItem(Cmd::AccurateBarriers, _("&Accurate barriers (Vulkan)"))->Check(config.vk_accurate_barriers);
	m_debugMenu->AppendSeparator();
#ifdef CEMU_DEBUG_ASSERT
	m_debugMenu->AppendCheckItem(Cmd::AudioAuxOnly, _("&Audio AUX only"))->Check(ActiveSettings::AudioOutputOnlyAux());
#endif
	m_debugMenu->Append(Cmd::LoggingWindowCmd, _("&Open logging window"));
	m_gdbstub_toggle = m_debugMenu->AppendCheckItem(Cmd::GdbStub, _("&Launch with GDB stub"));
	m_gdbstub_toggle->Check(g_gdbstub != nullptr);
	m_gdbstub_toggle->Enable(!m_game_launched);
	m_debugMenu->Append(Cmd::PpcThreads, _("&View PPC threads"));
	m_debugMenu->Append(Cmd::PpcDebugger, _("&View PPC debugger"));
	m_debugMenu->Append(Cmd::AudioDebugger, _("&View audio debugger"));
	m_debugMenu->Append(Cmd::TextureRelations, _("&View texture cache info"));
	m_debugMenu->Append(Cmd::DumpRam, _("&Dump current RAM"))->Enable(m_game_launched);
	m_debugMenu->Append(Cmd::DumpRpx, _("&Dump current RPX"))->Enable(m_game_launched);
	m_menuBar->Append(m_debugMenu, _("&Debug"));

	// Help (PixelU never updates itself from Cemu's servers, so the update item stays disabled)
	auto* helpMenu = new wxMenu();
	m_check_update_menu = helpMenu->Append(Cmd::CheckForUpdates, _("&Check for updates"));
	m_check_update_menu->Enable(false);
	helpMenu->AppendSeparator();
	helpMenu->Append(Cmd::About, _("&About PixelU"));
	m_menuBar->Append(helpMenu, _("&Help"));

	// docked into RetroPixels the menu is still built (its items hold state) but never attached
	SetMenuBar(RPEmbed::Enabled() ? nullptr : m_menuBar);
	m_menu_visible = !RPEmbed::Enabled();

	if (m_game_launched)
	{
		m_memorySearcherMenuItem->Enable(true);
		m_nfcMenu->Enable(Cmd::TouchNfcFile, true);
		// the renderer is set up by now; its debug output can't be switched any more
		m_loggingSubmenu->Enable(LogCmd(LogType::OpenGLLogging), false);
		m_loggingSubmenu->Enable(LogCmd(LogType::VulkanValidation), false);
		UpdateNFCMenu();
	}

	if (IsFullScreen())
		SetMenuVisible(false);
}
