// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "WindowSystem.h"
#include "util/crypto/aes128.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "config/LaunchSettings.h"
#include "input/InputManager.h"
#include "input/DefaultControllerProfile.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/SaveList.h"

#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/cpu_features.h"

#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"

#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cafe/OS/libs/vpad/vpad.h"

#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"

#if BOOST_OS_WINDOWS
#pragma comment(lib, "Dbghelp.lib")
#endif

#ifdef HAS_SDL
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#endif

#if BOOST_OS_LINUX
#include <sys/sysinfo.h>
#elif BOOST_OS_MACOS || BOOST_OS_BSD
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#if BOOST_OS_WINDOWS
// ask hybrid-graphics laptops for the discrete GPU
extern "C"
{
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}
#endif

std::atomic_bool g_isGPUInitFinished = false;

std::wstring executablePath; // Windows: full path of the running executable

namespace
{
	// Sets an environment variable from a "NAME=value" string. putenv may keep
	// the pointer instead of copying, so the strings are never freed.
	void setEnvironmentPermanently(const char* assignment)
	{
		static std::vector<std::unique_ptr<std::string>> s_storage;
		std::string* stored = s_storage.emplace_back(std::make_unique<std::string>(assignment)).get();
#if BOOST_OS_WINDOWS
		_putenv(stored->c_str());
#else
		putenv(stored->data());
#endif
	}

	// let the NVIDIA OpenGL driver keep its shader cache inside our cache folder
	void configureOpenGLDriverCache()
	{
#ifdef ENABLE_OPENGL
		const fs::path cacheDir = ActiveSettings::GetCachePath("shaderCache/driver/nvidia/");
		std::error_code ec;
		fs::create_directories(cacheDir, ec);
		const std::string assignment = "__GL_SHADER_DISK_CACHE_PATH=" + _pathToUtf8(cacheDir);
#if BOOST_OS_WINDOWS
		_wputenv(boost::nowide::widen(assignment).c_str()); // the path may contain non-ANSI characters
#else
		setEnvironmentPermanently(assignment.c_str());
#endif
		setEnvironmentPermanently("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1");
#endif
	}

	// implicit Vulkan layers known to cause trouble
	void disableProblematicVulkanLayers()
	{
#ifdef ENABLE_VULKAN
		setEnvironmentPermanently("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1");
		setEnvironmentPermanently("DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1");
#endif
	}

	// Windows: run from the executable's folder at raised priority
	void prepareWindowsProcess()
	{
#if BOOST_OS_WINDOWS
		std::wstring path(4096, L'\0');
		const DWORD length = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
		path.resize(length);
		executablePath = path;
		SetCurrentDirectoryW(fs::path(executablePath).parent_path().c_str());
		SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#endif
	}

	bool s_consoleAttached = false;
}

// startup shared by all front ends, after the command line and the folders are set
void CemuCommonInit()
{
	configureOpenGLDriverCache();
	disableProblematicVulkanLayers();
	AES128_init();
	// measures the timestamp counter frequency in the background, so start it early
	PPCTimer_init();

	prepareWindowsProcess();
	ExceptionHandler_Init();

	GetConfigHandle().Load();
	if (NetworkConfig::XMLExists())
		n_config.Load();

	// audio backends and graphic packs load in parallel with the input setup
	auto audioInit = std::async(std::launch::async, [] {
		IAudioAPI::InitializeStatic();
		IAudioInputAPI::InitializeStatic();
		return 0;
	});
	auto graphicPackInit = std::async(std::launch::async, [] {
		GraphicPack2::LoadAll();
		return 0;
	});
	DefaultControllerProfile_CreateIfMissing(); // PixelU: a fresh PixelU gets a working player 1 profile
	InputManager::instance().load();
	audioInit.wait();
	graphicPackInit.wait();

	CafeSystem::Initialize();

	CafeTitleList::Initialize(ActiveSettings::GetUserDataPath("title_list_cache.xml"));
	for (const std::string& gamePath : GetConfig().game_paths)
		CafeTitleList::AddScanPath(_utf8ToPath(gamePath));
	const fs::path mlcPath = ActiveSettings::GetMlcPath();
	if (!mlcPath.empty())
		CafeTitleList::SetMLCPath(mlcPath);
	CafeTitleList::Refresh();

	CafeSaveList::Initialize();
	if (!mlcPath.empty())
	{
		CafeSaveList::SetMLCPath(mlcPath);
		CafeSaveList::Refresh();
	}
}

void mainEmulatorLLE();
void ppcAsmTest();
void gx2CopySurfaceTest();
void ExpressionParser_test();
void FSTVolumeTest();
void CRCTest();

// self tests of various components (debug builds)
void UnitTests()
{
	ExpressionParser_test();
	gx2CopySurfaceTest();
	ppcAsmTest();
	FSTVolumeTest();
	CRCTest();
}

// Windows GUI programs have no console. When started from a console window,
// attach to it so that output written to stdout/stderr shows up there.
void requireConsole()
{
#if BOOST_OS_WINDOWS
	if (s_consoleAttached)
		return;
	const DWORD outputType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
	const bool redirected = outputType != FILE_TYPE_UNKNOWN && outputType != FILE_TYPE_CHAR;
	if (redirected)
	{
		s_consoleAttached = true; // output already goes to a file or pipe
		return;
	}
	if (AttachConsole(ATTACH_PARENT_PROCESS))
	{
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
		freopen("CONIN$", "r", stdin);
		s_consoleAttached = true;
	}
#endif
}

// removes the "<exe>.backup" an update leaves behind; tries for a few seconds at most
void HandlePostUpdate()
{
	fs::path backup = ActiveSettings::GetExecutablePath();
	backup.replace_extension(_utf8ToPath(_pathToUtf8(backup.extension()) + ".backup"));
	std::error_code ec;
	if (!fs::exists(backup, ec))
		return;
	for (int attempt = 0; attempt < 3; attempt++)
	{
		if (fs::remove(backup, ec))
			return;
		std::this_thread::sleep_for(std::chrono::seconds(1)); // the old process may still be exiting
	}
}

void ToolShaderCacheMerger();

namespace
{
	// parses the command line, then hands over to the window system
	template<typename... TArgs>
	int runApplication(TArgs&&... commandLine)
	{
		if (const std::optional<int> exitCode = LaunchSettings::HandleCommandline(std::forward<TArgs>(commandLine)...))
			return *exitCode;
		WindowSystem::Create();
		return 0;
	}

#if BOOST_OS_WINDOWS
	void prepareWindowsEntry()
	{
		if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE)))
			cemuLog_log(LogType::Force, "CoInitializeEx() failed");
#ifdef HAS_SDL
		SDL_SetMainReady();
#endif
	}
#endif
}

#if BOOST_OS_WINDOWS

// release builds (GUI subsystem)
int wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
	prepareWindowsEntry();
	return runApplication((const wchar_t*)lpCmdLine);
}

// debug builds (console subsystem)
int main(int argc, char* argv[])
{
	prepareWindowsEntry();
	return runApplication(argc, argv);
}

#else

int BreathOfTheWildChildProcessMain();

int main(int argc, char* argv[])
{
#if BOOST_OS_LINUX && defined(ENABLE_VULKAN)
	// helper process used to detect the RADV driver (see VulkanRenderer.cpp)
	if (getenv("CEMU_DETECT_RADV") != nullptr)
		return BreathOfTheWildChildProcessMain();
#endif
#if BOOST_OS_LINUX || BOOST_OS_BSD
	XInitThreads(); // Xlib is used from several threads
#endif
	return runApplication(argc, argv);
}

#endif

// read by external tools attached to the process
extern "C" DLLEXPORT uint64 gameMeta_getTitleId()
{
	return CafeSystem::GetForegroundTitleId();
}
