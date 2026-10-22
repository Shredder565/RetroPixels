// Original code for RetroPixels (PixelU). Not derived from Cemu.
// The wxWidgets implementation of the WindowSystem interface the emulator core uses.

#include "input/HotkeySettings.h"
#include "interface/WindowSystem.h"

#include "helpers/wxHelpers.h"

#if BOOST_OS_LINUX || BOOST_OS_BSD
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkwindow.h>
#include <gdk/gdkx.h>
#ifdef HAS_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#endif

#if BOOST_OS_MACOS
#include <Carbon/Carbon.h>
#endif

#include "wxgui/wxgui.h"
#include "wxgui/CemuApp.h"
#include "wxgui/MainWindow.h"
#include "wxgui/RPEmbed.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/CafeSystem.h"
#include "config/LaunchSettings.h"

WindowSystem::WindowInfo g_window_info{};

std::shared_mutex g_mutex;			// guards g_mainFrame against the window going away
MainWindow* g_mainFrame = nullptr;	// set by MainWindow itself

#if BOOST_OS_WINDOWS
void _wxLaunch()
{
	SetThreadName("MainThread_UI");
	wxEntry();
}
#endif

void WindowSystem::Create()
{
	SetThreadName("cemu");
#if BOOST_OS_WINDOWS
	// The GUI runs on a thread of its own on Windows: with wxWidgets on the process' first thread,
	// the folder picker (wxDirDialog::ShowModal) can hang inside the shell.
	std::thread guiThread(_wxLaunch);
	guiThread.join();
#else
	char* noArgs[1] = {nullptr};
	int argCount = 0;
	wxEntry(argCount, noArgs);
#endif
}

void WindowSystem::ShowErrorDialog(std::string_view message, std::string_view title, std::optional<WindowSystem::ErrorCategory> errorCategory)
{
	// PixelU: a graphic pack error does not stop the game, so under --rp-embed it only goes to the log
	// (no extra window next to the docked game)
	if (LaunchSettings::RPEmbedEnabled() && errorCategory == WindowSystem::ErrorCategory::GRAPHIC_PACKS)
	{
		cemuLog_log(LogType::Force, "{}", message);
		return;
	}
	const wxString caption = title.empty() ? wxString(wxASCII_STR(wxMessageBoxCaptionStr)) : wxString::FromUTF8(title);
	// PixelU: shown inside RetroPixels under --rp-embed
	RPEmbed::ShowMessage(wxString::FromUTF8(message), caption, wxOK | wxCENTRE | wxICON_ERROR);
}

WindowSystem::WindowInfo& WindowSystem::GetWindowInfo()
{
	return g_window_info;
}

namespace
{
	const char* RendererTag()
	{
		if (!g_renderer)
			return "";
		switch (g_renderer->GetType())
		{
		case RendererAPI::OpenGL: return "[OpenGL]";
		case RendererAPI::Vulkan: return "[Vulkan]";
		case RendererAPI::Metal: return "[Metal]";
		default: return "";
		}
	}

	const char* GpuVendorTag()
	{
		switch (LatteGPUState.glVendor)
		{
		case GLVENDOR_AMD: return "[AMD GPU]";
		case GLVENDOR_INTEL: return "[Intel GPU]";
		case GLVENDOR_NVIDIA: return "[NVIDIA GPU]";
		case GLVENDOR_APPLE: return "[Apple GPU]";
		default: return "[Generic]";
		}
	}

	std::string OnlineTag()
	{
		if (!ActiveSettings::IsOnlineEnabled())
			return {};
		switch (ActiveSettings::GetNetworkService())
		{
		case NetworkService::Nintendo: return " [Online]";
		case NetworkService::Pretendo: return " [Online-Pretendo]";
		case NetworkService::Custom: return " [Online-" + GetNetworkConfig().networkname.GetValue() + "]";
		default: return {};
		}
	}

	std::string RegionTag(CafeConsoleRegion region, uint16 version)
	{
		const char* prefix = "";
		if (region == CafeConsoleRegion::JPN)
			prefix = "JP ";
		else if (region == CafeConsoleRegion::USA)
			prefix = "US ";
		else if (region == CafeConsoleRegion::EUR)
			prefix = "EU ";
		return fmt::format(" [{}v{}]", prefix, version);
	}
}

void WindowSystem::UpdateWindowTitles(bool isIdle, bool isLoading, double fps)
{
	if (LaunchSettings::RPEmbedEnabled())
	{
		// PixelU: RetroPixels shows the details itself, the title is only "PixelU - <game name>"
		std::string embedTitle(EMULATOR_NAME);
		if (!isIdle)
		{
			const std::string gameName = CafeSystem::GetForegroundTitleName();
			if (!gameName.empty())
				embedTitle += " - " + gameName;
		}
		std::shared_lock guard(g_mutex);
		if (g_mainFrame)
			g_mainFrame->AsyncSetTitle(embedTitle);
		return;
	}

	std::string title(BUILD_VERSION_WITH_NAME_STRING);
	if (isIdle || isLoading)
	{
		if (isLoading && !isIdle)
			title += " - Loading...";
		if (g_mainFrame)
			g_mainFrame->AsyncSetTitle(title);
		return;
	}

	const uint64 titleId = CafeSystem::GetForegroundTitleId();
	title += fmt::format(" - FPS: {:.2f} {} {} [TitleId: {:08x}-{:08x}]", fps, RendererTag(), GpuVendorTag(), (uint32)(titleId >> 32), (uint32)titleId);
	title += OnlineTag();
	title += ' ';
	title += CafeSystem::GetForegroundTitleName();
	title += RegionTag(CafeSystem::GetForegroundTitleRegion(), CafeSystem::GetForegroundTitleVersion());

	std::shared_lock guard(g_mutex);
	if (!g_mainFrame)
		return;
	g_mainFrame->AsyncSetTitle(title);
	if (PadViewFrame* padWindow = g_mainFrame->GetPadView())
		padWindow->AsyncSetTitle(fmt::format("{} - FPS: {:.02f}", _("GamePad View").utf8_string(), fps));
}

void WindowSystem::GetWindowSize(int& w, int& h)
{
	w = g_window_info.width;
	h = g_window_info.height;
}

void WindowSystem::GetPadWindowSize(int& w, int& h)
{
	const bool open = g_window_info.pad_open;
	w = open ? g_window_info.pad_width.load() : 0;
	h = open ? g_window_info.pad_height.load() : 0;
}

void WindowSystem::GetWindowPhysSize(int& w, int& h)
{
	w = g_window_info.phys_width;
	h = g_window_info.phys_height;
}

void WindowSystem::GetPadWindowPhysSize(int& w, int& h)
{
	const bool open = g_window_info.pad_open;
	w = open ? g_window_info.phys_pad_width.load() : 0;
	h = open ? g_window_info.phys_pad_height.load() : 0;
}

double WindowSystem::GetWindowDPIScale()
{
	return g_window_info.dpi_scale;
}

double WindowSystem::GetPadDPIScale()
{
	if (!g_window_info.pad_open)
		return 1.0;
	return g_window_info.pad_dpi_scale.load();
}

bool WindowSystem::IsPadWindowOpen()
{
	return g_window_info.pad_open;
}

bool WindowSystem::IsKeyDown(uint32 key)
{
	return g_window_info.get_keystate(key);
}

namespace
{
	// platform key code of a PlatformKeyCodes value, 0 if there is none
	uint32 NativeKeyCode(WindowSystem::PlatformKeyCodes key)
	{
		using K = WindowSystem::PlatformKeyCodes;
#if BOOST_OS_WINDOWS
		constexpr uint32 lctrl = VK_LCONTROL, rctrl = VK_RCONTROL, tab = VK_TAB, esc = VK_ESCAPE;
#elif BOOST_OS_LINUX || BOOST_OS_BSD
		constexpr uint32 lctrl = GDK_KEY_Control_L, rctrl = GDK_KEY_Control_R, tab = GDK_KEY_Tab, esc = GDK_KEY_Escape;
#elif BOOST_OS_MACOS
		constexpr uint32 lctrl = kVK_Control, rctrl = kVK_RightControl, tab = kVK_Tab, esc = kVK_Escape;
#else
		constexpr uint32 lctrl = 0, rctrl = 0, tab = 0, esc = 0;
#endif
		switch (key)
		{
		case K::LCONTROL: return lctrl;
		case K::RCONTROL: return rctrl;
		case K::TAB: return tab;
		case K::ESCAPE: return esc;
		}
		return 0;
	}
}

bool WindowSystem::IsKeyDown(PlatformKeyCodes platformKey)
{
	const uint32 code = NativeKeyCode(platformKey);
	if (code == 0)
		return false;
	return IsKeyDown(code);
}

std::string WindowSystem::GetKeyCodeName(uint32 button)
{
#if BOOST_OS_WINDOWS
	LONG scanCode = MapVirtualKeyA((UINT)button, MAPVK_VK_TO_VSC_EX);
	if (HIBYTE(scanCode))
		scanCode |= 0x100;
	// MapVirtualKey loses the extended-key flag of these, GetKeyNameText needs it to name them right
	static constexpr UINT s_extendedKeys[] = {VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, VK_PRIOR, VK_NEXT, VK_END,
											  VK_HOME, VK_INSERT, VK_DELETE, VK_DIVIDE, VK_NUMLOCK};
	if (std::find(std::begin(s_extendedKeys), std::end(s_extendedKeys), (UINT)button) != std::end(s_extendedKeys))
		scanCode |= 0x100;
	char name[128];
	if (GetKeyNameTextA(scanCode << 16, name, (int)std::size(name)) != 0)
		return name;
	return fmt::format("key_{}", button);
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	return gdk_keyval_name(button);
#else
	return fmt::format("key_{}", button);
#endif
}

bool WindowSystem::InputConfigWindowHasFocus()
{
	return g_inputConfigWindowHasFocus;
}

void WindowSystem::NotifyGameLoaded()
{
	std::shared_lock guard(g_mutex);
	if (!g_mainFrame)
		return;
	g_mainFrame->OnGameLoaded();
	g_mainFrame->UpdateSettingsAfterGameLaunch();
}

void WindowSystem::NotifyGameExited()
{
	std::shared_lock guard(g_mutex);
	if (g_mainFrame)
		g_mainFrame->RestoreSettingsAfterGameExited();
}

void WindowSystem::RefreshGameList()
{
	std::shared_lock guard(g_mutex);
	if (g_mainFrame)
		g_mainFrame->RequestGameListRefresh();
}

void WindowSystem::CaptureInput(const ControllerState& currentState, const ControllerState& lastState)
{
	HotkeySettings::CaptureInput(currentState, lastState);
}

bool WindowSystem::IsFullScreen()
{
	return g_window_info.is_fullscreen;
}
