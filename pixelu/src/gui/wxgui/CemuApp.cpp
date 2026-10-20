// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/CemuApp.h"
#include "wxCemuConfig.h"
#include "wxgui/MainWindow.h"
#include "wxgui/wxgui.h"
#include "config/CemuConfig.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#endif
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "wxgui/GettingStartedDialog.h"
#include "wxgui/RPEmbed.h"
#include "input/InputManager.h"
#include "input/api/SDL/SDLControllerProvider.h"
#include "wxgui/helpers/wxHelpers.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "wxgui/input/HotkeySettings.h"
#include "wxgui/debugger/DebuggerWindow2.h"
#include <wx/language.h>

#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
#include "wxgui/helpers/wxWayland.h"
#endif
#if __WXGTK__
#include <glib.h>
#endif

#include <wx/image.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/clipbrd.h>
#include <wx/timer.h>
#include "wxHelper.h"

#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/CafeSystem.h"

wxIMPLEMENT_APP_NO_MAIN(CemuApp);

// wxWindowSystem.cpp
extern WindowSystem::WindowInfo g_window_info;
extern std::shared_mutex g_mutex;

// main.cpp
void UnitTests();
void CemuCommonInit();
void HandlePostUpdate();

// Strings that only appear in wxWidgets' own dialogs or in data files, listed so that gettext extracts them
void unused_translation_dummy()
{
	const wxString strings[] = {
		_("Browse"), _("Select a file"), _("Select a directory"),
		// console languages
		_("Japanese"), _("English"), _("French"), _("German"), _("Italian"), _("Spanish"), _("Chinese"),
		_("Korean"), _("Dutch"), _("Portugese"), _("Russian"), _("Taiwanese"), _("unknown"),
	};
	(void)strings;
}

// ---- where settings, saves and caches go ----

namespace
{
	struct AppPaths
	{
		bool portable = false;
		fs::path exe;
		fs::path userData, config, cache, data;

		void UseForEverything(const fs::path& dir)
		{
			userData = config = cache = dir;
		}
	};
}

#if BOOST_OS_WINDOWS
#include <shlobj.h>
fs::path GetAppDataRoamingPath()
{
	PWSTR folder = nullptr;
	const HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &folder);
	fs::path result;
	if (SUCCEEDED(hr))
		result = _utf8ToPath(boost::nowide::narrow(folder));
	CoTaskMemFree(folder);
	return result;
}

void CemuApp::DeterminePaths(std::set<fs::path>& failedWriteAccess)
{
	std::error_code ec;
	AppPaths paths;
	auto standardPaths = wxStandardPaths::Get();
	paths.exe = wxHelper::MakeFSPath(standardPaths.GetExecutablePath());
	const fs::path exeDir = paths.exe.parent_path();
	paths.data = exeDir; // resources always sit next to the executable on Windows
#ifdef CEMU_ALLOW_PORTABLE
	if (fs::is_directory(exeDir / "portable", ec))
	{
		paths.portable = true;
		paths.UseForEverything(exeDir / "portable");
	}
#endif
	if (!paths.portable)
	{
		paths.UseForEverything(GetAppDataRoamingPath() / "PixelU");
		// older installations kept everything next to the executable: a settings.xml there means portable
		if (fs::exists(exeDir / "settings.xml", ec))
		{
			paths.portable = true;
			paths.UseForEverything(exeDir);
		}
	}
	ActiveSettings::SetPaths(paths.portable, paths.exe, paths.userData, paths.config, paths.cache, paths.data, failedWriteAccess);
}
#endif

#if BOOST_OS_LINUX || BOOST_OS_BSD
void CemuApp::DeterminePaths(std::set<fs::path>& failedWriteAccess)
{
	std::error_code ec;
	AppPaths paths;
	auto standardPaths = wxStandardPaths::Get();
	paths.exe = wxHelper::MakeFSPath(standardPaths.GetExecutablePath());
	// inside an AppImage the executable path is the temporary mount, APPIMAGE has the real file
	wxString appImage;
	if (wxGetEnv("APPIMAGE", &appImage))
		paths.exe = wxHelper::MakeFSPath(appImage);
	const fs::path exeDir = paths.exe.parent_path();
#ifdef CEMU_ALLOW_PORTABLE
	if (fs::is_directory(exeDir / "portable", ec))
	{
		paths.portable = true;
		paths.UseForEverything(exeDir / "portable");
		paths.data = exeDir; // resources and default game profiles next to the executable
	}
#endif
	if (!paths.portable)
	{
		SetAppName("PixelU");
		const wxString appName = GetAppName();
		standardPaths.SetFileLayout(wxStandardPaths::FileLayout::FileLayout_XDG);
		const wxString home = wxFileName::GetHomeDir();
		// $name when it is set and not empty, else the XDG default below the home folder
		auto xdgDir = [&home](const wxString& name, const wxString& underHome) {
			wxString value;
			if (wxGetEnv(name, &value) && !value.empty())
				return value;
			return home + underHome;
		};
		paths.userData = (xdgDir(wxS("XDG_DATA_HOME"), wxS("/.local/share")) + "/" + appName).ToStdString();
		paths.config = (xdgDir(wxS("XDG_CONFIG_HOME"), wxS("/.config")) + "/" + appName).ToStdString();
		paths.data = standardPaths.GetDataDir().ToStdString();
		paths.cache = fs::path(standardPaths.GetUserDir(wxStandardPaths::Dir::Dir_Cache).ToStdString()) / appName.ToStdString();
	}
	ActiveSettings::SetPaths(paths.portable, paths.exe, paths.userData, paths.config, paths.cache, paths.data, failedWriteAccess);
}
#endif

#if BOOST_OS_MACOS
void CemuApp::DeterminePaths(std::set<fs::path>& failedWriteAccess)
{
	std::error_code ec;
	AppPaths paths;
	auto standardPaths = wxStandardPaths::Get();
	paths.exe = wxHelper::MakeFSPath(standardPaths.GetExecutablePath());
	const fs::path exeDir = paths.exe.parent_path();
	// in an app bundle (X.app/Contents/MacOS/exe) the portable folder sits next to the bundle
	const fs::path bundle = exeDir.parent_path().parent_path();
	const fs::path portableDir = bundle.extension() == ".app" ? bundle.parent_path() / "portable" : exeDir / "portable";
#ifdef CEMU_ALLOW_PORTABLE
	if (fs::is_directory(portableDir, ec))
	{
		paths.portable = true;
		paths.UseForEverything(portableDir);
		paths.data = exeDir;
	}
#endif
	if (!paths.portable)
	{
		SetAppName("PixelU");
		const wxString appName = GetAppName();
		paths.userData = paths.config = standardPaths.GetUserDataDir().ToStdString();
		paths.data = standardPaths.GetDataDir().ToStdString();
		paths.cache = fs::path(standardPaths.GetUserDir(wxStandardPaths::Dir::Dir_Cache).ToStdString()) / appName.ToStdString();
	}
	ActiveSettings::SetPaths(paths.portable, paths.exe, paths.userData, paths.config, paths.cache, paths.data, failedWriteAccess);
}
#endif

// ---- mlc folder ----

namespace
{
	[[noreturn]] void QuitWithMlcError(const wxString& text, const fs::path& mlc)
	{
		RPEmbed::ShowMessage(formatWxString(text, wxHelper::FromPath(mlc)), _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		exit(0);
	}
}

void CemuApp::InitializeNewMLCOrFail(fs::path mlc)
{
	if (CreateDefaultMLCFiles(mlc))
		return;
	cemu_assert_debug(!ActiveSettings::IsCustomMlcPath()); // a new installation has no custom path yet
	if (ActiveSettings::IsCommandLineMlcPath() || ActiveSettings::IsCustomMlcPath())
		QuitWithMlcError(_("PixelU failed to write to the custom mlc directory.\nThe path is:\n{}"), mlc);
	QuitWithMlcError(_("PixelU failed to write to the mlc directory.\nThe path is:\n{}"), mlc);
}

void CemuApp::InitializeExistingMLCOrFail(fs::path mlc)
{
	if (CreateDefaultMLCFiles(mlc))
		return;
	if (!ActiveSettings::IsCommandLineMlcPath() && !ActiveSettings::IsCustomMlcPath())
		QuitWithMlcError(_("PixelU failed to write to the default mlc directory.\nThe path is:\n{}"), mlc);
	// a path from the command line can't be changed from here
	if (ActiveSettings::IsCommandLineMlcPath())
		QuitWithMlcError(_("PixelU failed to write to the custom mlc directory.\nThe path is:\n{}"), mlc);

	// the custom path from the settings: offer to go back to the default one
	const wxString question = formatWxString(_("PixelU failed to write to the custom mlc directory.\n\nThe path is:\n{}\n\nPixelU cannot start without a valid mlc path. Do you want to reset the path? You can later change it again in the General Settings."),
											 _pathToUtf8(mlc));
	if (RPEmbed::Enabled())
	{
		// PixelU: no question window of its own inside RetroPixels; started on its own it asks
		RPEmbed::ShowMessage(question, _("Error"), wxOK | wxICON_WARNING);
		exit(0);
	}
	wxMessageDialog dialog(nullptr, question, _("Error"), wxCENTRE | wxYES_NO | wxICON_WARNING);
	dialog.SetYesNoLabels(_("Reset path"), _("Exit"));
	if (dialog.ShowModal() == wxID_NO)
		exit(0);
	GetConfig().mlc_path = "";
	GetConfigHandle().Save();
}

std::string TranslationCallback(std::string_view msgId)
{
	return wxGetTranslation(wxString::FromUTF8(msgId)).utf8_string();
}

// ---- start-up and shutdown ----

bool CemuApp::OnInit()
{
#if __WXGTK__
	GTKSuppressDiagnostics(G_LOG_LEVEL_MASK & ~G_LOG_FLAG_FATAL);
#endif
	RPEmbed::RouteWxLog(); // PixelU: no wxWidgets log popups under --rp-embed

	std::set<fs::path> failedWriteAccess;
	DeterminePaths(failedWriteAccess);
	CreateDefaultCemuFiles();

	const fs::path settingsFile = ActiveSettings::GetConfigPath("settings.xml");
	GetConfigHandle().SetFilename(settingsFile.generic_wstring());
	wxCemuConfig& guiConfig = GetWxGUIConfig();
	std::error_code ec;
	const bool firstStart = !fs::exists(settingsFile, ec);

	NetworkConfig::LoadOnce();
	sint32 language = wxLANGUAGE_DEFAULT;
	if (!firstStart)
	{
		GetConfigHandle().Load();
		language = guiConfig.language.GetValue();
	}
	if (language == wxLANGUAGE_DEFAULT)
		language = wxLocale::GetSystemLanguage();
	LocalizeUI(static_cast<wxLanguage>(language));
	SetTranslationCallback(TranslationCallback);

#if __WXMSW__
	const int theme = guiConfig.msw_theme.GetValue();
	if (theme == static_cast<int>(MSWThemeOption::kAuto))
		MSWEnableDarkMode(DarkMode_Auto);
	else if (theme == static_cast<int>(MSWThemeOption::kDark))
		MSWEnableDarkMode(DarkMode_Always);
	// tooltips stay up as long as Windows allows
	wxToolTip::SetDelay(-1);
	wxToolTip::SetAutoPop(MAKELPARAM(std::numeric_limits<short>::max(), 0));
#endif

	for (const fs::path& path : failedWriteAccess)
		RPEmbed::ShowMessage(formatWxString(_("PixelU can't write to {}!"), wxHelper::FromPath(path)), _("Warning"), wxOK | wxCENTRE | wxICON_EXCLAMATION);

	if (firstStart)
	{
		// PixelU: no Getting Started dialog while RetroPixels shows the game in its window
		if (!RPEmbed::Enabled())
		{
			GettingStartedDialog gettingStarted(nullptr);
			gettingStarted.ShowModal();
		}
		// write settings.xml now even if a dialog already did
		GetConfigHandle().Save();
		// after the dialog, which may have picked another mlc folder
		InitializeNewMLCOrFail(ActiveSettings::GetMlcPath());
	}
	else
	{
		InitializeExistingMLCOrFail(ActiveSettings::GetMlcPath());
	}

	ActiveSettings::Init(); // loads the online certificates
	HandlePostUpdate();

	LatteOverlay_init();
#ifdef CEMU_DEBUG_ASSERT
	UnitTests();
#endif

#if BOOST_OS_MACOS
	SDLControllerProvider::InitSDL();
#endif
	CemuCommonInit();

#if BOOST_OS_MACOS
	// SDL events have to be pumped from the main thread here
	constexpr int kSdlPumpIntervalMs = 5;
	Bind(wxEVT_TIMER, &CemuApp::OnSDLEventPumpTimer, this);
	m_sdlEventPumpTimer = new wxTimer(this);
	m_sdlEventPumpTimer->Start(kSdlPumpIntervalMs, wxTIMER_CONTINUOUS);
#endif

	wxInitAllImageHandlers();

	// named colours the dialogs use for status texts
	wxTheColourDatabase->AddColour("ERROR", wxColour(0xCC, 0, 0));
	wxTheColourDatabase->AddColour("SUCCESS", wxColour(0, 0xbb, 0));

#if BOOST_OS_WINDOWS
	// refuse to run under WiiU USB Helper
	const fs::path parentProcess = GetParentProcess();
	if (parentProcess.has_filename() && boost::icontains(parentProcess.filename().generic_string(), "WiiU_USB_Helper"))
		__fastfail(0);
#endif
#ifdef ENABLE_VULKAN
	InitializeGlobalVulkan();
#endif

	Bind(wxEVT_ACTIVATE_APP, &CemuApp::ActivateApp, this);

	m_mainFrame = new MainWindow();
	RPEmbed::AttachToParent(m_mainFrame); // PixelU: --rp-parent

	std::unique_lock windowLock(g_mutex);
	g_window_info.app_active = true;

	HotkeySettings::Init(m_mainFrame);

	SetTopWindow(m_mainFrame);
	RPEmbed::BeforeShow(m_mainFrame); // PixelU: under --rp-embed it waits off-screen to be docked
	m_mainFrame->Show();

#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
	if (wxWlIsWaylandWindow(m_mainFrame))
		wxWlSetAppId(m_mainFrame, "io.github.shredder565.PixelU"); // Wayland app id (a desktop file would use the same name)
#endif

	return true;
}

int CemuApp::OnExit()
{
#if BOOST_OS_MACOS
	if (m_sdlEventPumpTimer)
	{
		m_sdlEventPumpTimer->Stop();
		Unbind(wxEVT_TIMER, &CemuApp::OnSDLEventPumpTimer, this);
		delete m_sdlEventPumpTimer;
		m_sdlEventPumpTimer = nullptr;
	}
#endif
	wxApp::OnExit();
	wxTheClipboard->Flush();
	InputManager::instance().Shutdown();
	LatteRecorder::Stop(); // PixelU: finish the recording, ExitProcess below skips static destructors

	// a game started from the command line hands its exit code on
	const bool launchedFromCommandLine = LaunchSettings::GetLoadFile() || LaunchSettings::GetLoadTitleID();
	const auto titleResult = CafeSystem::GetForegroundTitleReturnStatus();
	const int exitCode = (launchedFromCommandLine && titleResult) ? *titleResult : 0;
#if BOOST_OS_MACOS
	SDLControllerProvider::ShutdownSDL();
#endif

	if (m_restartExecutable && !m_restartExecutable->empty() && fs::exists(*m_restartExecutable))
	{
		const std::string target = _pathToUtf8(*m_restartExecutable);
#if BOOST_OS_WINDOWS
		std::wstring commandLine = L"\"" + boost::nowide::widen(target) + L"\"";
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process);
#elif BOOST_OS_LINUX || BOOST_OS_MACOS
		execlp(target.c_str(), target.c_str(), (char*)nullptr);
#endif
	}
#if BOOST_OS_WINDOWS
	ExitProcess(exitCode);
#else
	_Exit(exitCode);
#endif
}

#if BOOST_OS_MACOS
void CemuApp::OnSDLEventPumpTimer(wxTimerEvent& event)
{
	// elsewhere SDLControllerProvider polls SDL on a thread of its own
	SDLControllerProvider::PumpSDLEvents();
}
#endif

#if BOOST_OS_WINDOWS
void DumpThreadStackTrace();
#endif

void CemuApp::OnAssertFailure(const wxChar* file, int line, const wxChar* func, const wxChar* cond, const wxChar* msg)
{
	auto utf8 = [](const wxChar* s) { return wxString(s).utf8_string(); };
	cemuLog_createLogFile(false);
	cemuLog_log(LogType::Force, "Encountered wxWidgets assert!");
	cemuLog_log(LogType::Force, "File: {0} Line: {1}", utf8(file), line);
	cemuLog_log(LogType::Force, "Func: {0} Cond: {1}", utf8(func), utf8(cond));
	cemuLog_log(LogType::Force, "Message: {}", utf8(msg));
#if BOOST_OS_WINDOWS
	DumpThreadStackTrace();
#endif
	cemu_assert_debug(false);
}

int CemuApp::FilterEvent(wxEvent& event)
{
	const wxEventType type = event.GetEventType();

	// every key press anywhere in the application is recorded for the emulated input
	if (type == wxEVT_KEY_DOWN || type == wxEVT_KEY_UP)
	{
		const auto& key = static_cast<wxKeyEvent&>(event);
		g_window_info.set_keystate(fix_raw_keycode(key.GetRawKeyCode(), key.GetRawKeyFlags()), type == wxEVT_KEY_DOWN);
	}
	else if (type == wxEVT_ACTIVATE_APP)
	{
		if (!static_cast<wxActivateEvent&>(event).GetActive())
			g_window_info.set_keystatesup();
	}
	else if (type == wxEVT_KILL_FOCUS && RPEmbed::Enabled())
	{
		// PixelU: docked into another application's window there is no app deactivation when the keyboard
		// moves to that application, only a focus loss to a window outside PixelU
		if (static_cast<wxFocusEvent&>(event).GetWindow() == nullptr)
			g_window_info.set_keystatesup();
	}

	// keep debugger_focused telling whether the debugger window (or anything inside it) is focused
	if (!s_debuggerWindow)
	{
		g_window_info.debugger_focused = false;
	}
	else if (type == wxEVT_SET_FOCUS || type == wxEVT_ACTIVATE)
	{
		wxWindow* focused = wxDynamicCast(event.GetEventObject(), wxWindow);
		if (focused && type == wxEVT_ACTIVATE && !static_cast<wxActivateEvent&>(event).GetActive())
			focused = nullptr;
		if (focused)
		{
			bool insideDebugger = false;
			for (wxWindow* w = focused; w; w = w->GetParent())
			{
				if (w == s_debuggerWindow)
					insideDebugger = true;
			}
			g_window_info.debugger_focused = insideDebugger;
		}
	}

	return wxApp::FilterEvent(event);
}

// ---- languages ----

std::vector<const wxLanguageInfo*> CemuApp::GetLanguages() const
{
	std::vector<const wxLanguageInfo*> languages;
	languages.reserve(m_availableTranslations.size() + 1);
	languages.push_back(wxLocale::GetLanguageInfo(wxLANGUAGE_ENGLISH));
	languages.insert(languages.end(), m_availableTranslations.begin(), m_availableTranslations.end());
	return languages;
}

void CemuApp::LocalizeUI(wxLanguage languageToUse)
{
	auto translations = std::make_unique<wxTranslations>();
	m_availableTranslations = GetAvailableTranslationLanguages(translations.get());

	const bool haveCatalog = std::any_of(m_availableTranslations.begin(), m_availableTranslations.end(),
										 [languageToUse](const wxLanguageInfo* info) { return info->Language == languageToUse; });
	if (languageToUse != wxLANGUAGE_DEFAULT && !haveCatalog)
		return; // stays English

	translations->SetLanguage(languageToUse);
	translations->AddCatalog("cemu");
	if (translations->IsLoaded("cemu") && wxLocale::IsAvailable(languageToUse))
		m_locale.Init(languageToUse);
	// after wxLocale::Init, which installs a wxTranslations of its own that this one replaces
	wxTranslations::Set(translations.release());
}

std::vector<const wxLanguageInfo*> CemuApp::GetAvailableTranslationLanguages(wxTranslations* translationsMgr)
{
	wxFileTranslationsLoader::AddCatalogLookupPathPrefix(wxHelper::FromPath(ActiveSettings::GetDataPath("resources")));
	std::vector<const wxLanguageInfo*> found;
	for (const wxString& name : translationsMgr->GetAvailableTranslations("cemu"))
	{
		if (const wxLanguageInfo* info = wxLocale::FindLanguageInfo(name))
			found.push_back(info);
	}
	return found;
}

// ---- mlc and data folders ----

bool CemuApp::CheckMLCPath(const fs::path& mlc)
{
	std::error_code ec;
	return fs::exists(mlc, ec) && fs::exists(mlc / "usr", ec) && fs::exists(mlc / "sys", ec);
}

bool CemuApp::CreateDefaultMLCFiles(const fs::path& mlc)
{
	const char* const folders[] = {
		"",
		"sys",
		"usr",
		"usr/title/00050000", // games
		"usr/title/0005000c", // DLC
		"usr/title/0005000e", // updates
		"usr/save/00050010/1004a000/user/common/db", // Mii Maker saves (JPN, USA, EUR)
		"usr/save/00050010/1004a100/user/common/db",
		"usr/save/00050010/1004a200/user/common/db",
		"sys/title/0005001b/1005c000/content", // system language data
	};
	for (const char* folder : folders)
	{
		const fs::path dir = *folder ? mlc / folder : mlc;
		std::error_code ec;
		if (!fs::exists(dir, ec) && !fs::create_directories(dir, ec))
			return false;
	}

	try
	{
		const fs::path contentDir = fs::path(mlc).append("sys/title/0005001b/1005c000/content");

		const fs::path languageList = fs::path(contentDir).append("language.txt");
		if (!fs::exists(languageList))
		{
			std::ofstream out(languageList);
			if (out.is_open())
			{
				for (const char* code : {"ja", "en", "fr", "de", "it", "es", "zh", "ko", "nl", "pt", "ru", "zh"})
					out << '"' << code << "\"," << std::endl;
				out.flush();
				out.close();
			}
		}

		const fs::path countryList = fs::path(contentDir).append("country.txt");
		if (!fs::exists(countryList))
		{
			std::ofstream out(countryList);
			for (sint32 index = 0; index < NCrypto::GetCountryCount(); index++)
			{
				const char* code = NCrypto::GetCountryAsString(index);
				if (boost::iequals(code, "NN"))
					out << "NULL," << std::endl;
				else
					out << '"' << code << "\"," << std::endl;
			}
			out.flush();
			out.close();
		}

		// finally prove the folder is writable
		const fs::path probe = fs::path(mlc).append("writetestdummy");
		std::ofstream probeFile(probe);
		if (!probeFile.is_open())
			return false;
		probeFile.close();
		fs::remove(probe);
	}
	catch (const std::exception&)
	{
		return false;
	}
	return true;
}

void CemuApp::CreateDefaultCemuFiles()
{
	try
	{
		for (const fs::path& dir : {ActiveSettings::GetConfigPath("controllerProfiles"), ActiveSettings::GetUserDataPath("memorySearcher")})
		{
			if (!fs::exists(dir))
				fs::create_directories(dir);
		}
	}
	catch (const std::exception& ex)
	{
		wxString text = formatWxString(_("Couldn't create a required PixelU directory or file!\n\nError: {0}"), ex.what());
#if BOOST_OS_WINDOWS
		if (const DWORD lastError = GetLastError(); lastError != ERROR_SUCCESS)
			text << fmt::format("\n\n{}", GetSystemErrorMessage(lastError));
#endif
		RPEmbed::ShowMessage(text, _("Error"), wxOK | wxCENTRE | wxICON_ERROR);
		exit(0);
	}
}

void CemuApp::ActivateApp(wxActivateEvent& event)
{
	g_window_info.app_active = event.GetActive();
	event.Skip();
}

void CemuApp::RequestRestart(fs::path executablePath)
{
	m_restartExecutable = std::move(executablePath);
	CallAfter([this]() {
		if (m_mainFrame && !m_mainFrame->IsBeingDeleted())
			m_mainFrame->Close(true);
	});
}
