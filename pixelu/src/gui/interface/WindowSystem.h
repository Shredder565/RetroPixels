// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "input/api/ControllerState.h"

// The emulator core talks to whatever GUI hosts it only through this namespace.
// The wxWidgets front end (gui/wxgui/wxWindowSystem.cpp) provides the functions.
namespace WindowSystem
{
	// native handles of one window or canvas, handed to the renderers
	struct WindowHandleInfo
	{
		enum class Backend
		{
			X11,
			Wayland,
			Cocoa,
			Windows,
		} backend;
		void* display = nullptr; // X11 Display*, wl_display*; unused elsewhere
		void* surface = nullptr; // Window, wl_surface*, HWND or NSView*
	};

	// keys the core asks about without knowing the platform's key codes
	enum struct PlatformKeyCodes : uint32
	{
		LCONTROL,
		RCONTROL,
		TAB,
		ESCAPE,
	};

	// State shared between the GUI thread and the emulator threads
	struct WindowInfo
	{
		std::atomic_bool app_active; // the application has the focus

		// main window client area, in logical and in physical pixels
		std::atomic_int32_t width, height;
		std::atomic_int32_t phys_width, phys_height;
		std::atomic<double> dpi_scale;

		// separate GamePad window
		std::atomic_bool pad_open;
		std::atomic_int32_t pad_width, pad_height;
		std::atomic_int32_t phys_pad_width, phys_pad_height;
		std::atomic<double> pad_dpi_scale;

		// GamePad window geometry to restore when it is un-maximized
		std::atomic_bool pad_maximized = false;
		std::atomic_int32_t restored_pad_x = -1;
		std::atomic_int32_t restored_pad_y = -1;
		std::atomic_int32_t restored_pad_width = -1;
		std::atomic_int32_t restored_pad_height = -1;

		std::atomic_bool is_fullscreen;
		std::atomic_bool debugger_focused;

		// keyboard state by platform key code
		void set_keystate(uint32 keycode, bool state)
		{
			std::scoped_lock guard(m_keyLock);
			m_keys[keycode] = state;
		}

		bool get_keystate(uint32 keycode)
		{
			std::scoped_lock guard(m_keyLock);
			const auto it = m_keys.find(keycode);
			return it != m_keys.end() && it->second;
		}

		// release every key (focus lost)
		void set_keystatesup()
		{
			std::scoped_lock guard(m_keyLock);
			for (auto& entry : m_keys)
				entry.second = false;
		}

		// calls f(const std::pair<const uint32, bool>&) for every key seen so far
		template<typename fn>
		void iter_keystates(fn f)
		{
			std::scoped_lock guard(m_keyLock);
			for (const auto& entry : m_keys)
				f(entry);
		}

		WindowHandleInfo window_main;
		WindowHandleInfo window_pad;

		// render canvases inside those windows
		WindowHandleInfo canvas_main;
		WindowHandleInfo canvas_pad;

	  private:
		std::mutex m_keyLock;
		std::unordered_map<uint32, bool> m_keys;
	};

	enum class ErrorCategory
	{
		KEYS_TXT_CREATION = 0,
		GRAPHIC_PACKS = 1,
	};

	void ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory> errorCategory = {});
	inline void ShowErrorDialog(std::string_view message, std::optional<ErrorCategory> errorCategory = {})
	{
		ShowErrorDialog(message, std::string_view{}, errorCategory);
	}

	void Create();

	WindowInfo& GetWindowInfo();

	void UpdateWindowTitles(bool isIdle, bool isLoading, double fps);
	void GetWindowSize(int& w, int& h);
	void GetPadWindowSize(int& w, int& h);
	void GetWindowPhysSize(int& w, int& h);
	void GetPadWindowPhysSize(int& w, int& h);
	double GetWindowDPIScale();
	double GetPadDPIScale();
	bool IsPadWindowOpen();
	bool IsKeyDown(uint32 key);
	bool IsKeyDown(PlatformKeyCodes key);
	std::string GetKeyCodeName(uint32 key);

	bool InputConfigWindowHasFocus();

	void NotifyGameLoaded();
	void NotifyGameExited();

	void RefreshGameList();

	bool IsFullScreen();

	void CaptureInput(const ControllerState& currentState, const ControllerState& lastState);
}; // namespace WindowSystem
