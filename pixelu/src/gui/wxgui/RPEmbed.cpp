// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/RPEmbed.h"
#include "config/LaunchSettings.h"

#include <wx/log.h>
#include <wx/msgdlg.h>

#include <atomic>
#include <chrono>
#include <functional>

bool RPEmbed::Enabled()
{
	return LaunchSettings::RPEmbedEnabled();
}

long RPEmbed::FrameStyle(long defaultStyle)
{
	if (!Enabled())
		return defaultStyle;
	// resizable, with a caption and a close button only
	long embedStyle = wxCAPTION | wxSYSTEM_MENU | wxCLOSE_BOX | wxRESIZE_BORDER | wxCLIP_CHILDREN;
#if BOOST_OS_WINDOWS
	// WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt+Tab while it waits to be docked,
	// and wxWidgets then shows it without activating it
	embedStyle |= wxFRAME_TOOL_WINDOW;
#endif
	return embedStyle;
}

#if BOOST_OS_WINDOWS
namespace
{
	using SteadyClock = std::chrono::steady_clock;

	constexpr int kOffscreenPos = -32000;
	constexpr auto kDockTimeout = std::chrono::seconds(20);
	constexpr int kPollMs = 100;

	// the main frame, once AttachToParent or BeforeShow saw it
	wxFrame* s_mainFrame = nullptr;
	std::atomic<HWND> s_mainHwnd{nullptr};
	// steady_clock ticks after which an undocked window gives up waiting (0 = no wait)
	std::atomic<SteadyClock::rep> s_giveUpAt{0};
	// set once the window was put on screen as an ordinary window
	std::atomic_bool s_standalone{false};

	void Remember(wxFrame* frame, HWND hwnd)
	{
		s_mainFrame = frame;
		s_mainHwnd = hwnd;
	}

	bool WaitTimedOut()
	{
		return SteadyClock::now().time_since_epoch().count() >= s_giveUpAt.load();
	}

	// For the Vulkan picture RetroPixels cannot use a child window inside its Edge/Chrome based window;
	// it makes PixelU's window a frameless window owned by its own window instead, kept above the page.
	HWND OwnerInOtherProcess(HWND hwnd)
	{
		const HWND owner = GetWindow(hwnd, GW_OWNER);
		if (!owner)
			return nullptr;
		DWORD ownerProcess = 0;
		if (!GetWindowThreadProcessId(owner, &ownerProcess) || ownerProcess == GetCurrentProcessId())
			return nullptr;
		return owner;
	}

	// docked: a child window (WS_CHILD or any parent but the desktop) or owned by another process' window
	bool IsDockedWindow(HWND hwnd)
	{
		if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0)
			return true;
		if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow())
			return true;
		return OwnerInOtherProcess(hwnd) != nullptr;
	}

	// the window PixelU is docked into
	HWND HostOf(HWND hwnd)
	{
		if (const HWND owner = OwnerInOtherProcess(hwnd))
			return owner;
		return GetAncestor(hwnd, GA_PARENT);
	}

	// Nobody docked it in time: make it the window it would be without --rp-embed (centred, on screen,
	// with a taskbar button). Only once.
	void BecomeStandalone()
	{
		wxFrame* frame = s_mainFrame;
		const HWND hwnd = s_mainHwnd.load();
		if (!frame || !hwnd || frame->IsBeingDeleted() || IsDockedWindow(hwnd))
			return;
		if (s_standalone.exchange(true))
			return;
		cemuLog_log(LogType::Force, "--rp-embed: the window wasn't docked within {} seconds, showing it on its own", (int)kDockTimeout.count());
		frame->Hide();
		frame->SetWindowStyleFlag(frame->GetWindowStyleFlag() & ~wxFRAME_TOOL_WINDOW);
		frame->Centre();
		frame->Show();
		frame->Raise();
	}

	// Blocks until the window is docked, shown standalone, gone or the wait timed out. On the window's own
	// thread it keeps answering the messages RetroPixels sends while docking it (SetParent, SetWindowPos),
	// and nothing else: no wx events are dispatched meanwhile.
	void BlockUntilDocked(HWND hwnd, bool onWindowThread)
	{
		while (IsWindow(hwnd) && !IsDockedWindow(hwnd) && !s_standalone && !WaitTimedOut())
		{
			if (!onWindowThread)
			{
				Sleep(20);
				continue;
			}
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_SENDMESSAGE);
			MSG peeked;
			PeekMessageW(&peeked, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
		}
		if (onWindowThread && IsWindow(hwnd) && !IsDockedWindow(hwnd))
			BecomeStandalone();
	}

	UINT MessageBoxIcon(long style)
	{
		if (style & wxICON_INFORMATION)
			return MB_ICONINFORMATION;
		if (style & wxICON_WARNING)
			return MB_ICONWARNING;
		return MB_ICONERROR;
	}

	void ShowOwnedMessageBox(const wxString& message, const wxString& caption, long style, bool alreadyDeferred)
	{
		const HWND hwnd = s_mainHwnd.load();
		if (!hwnd || !IsWindow(hwnd))
			return; // no game window yet: the log line is all there is
		const bool onWindowThread = GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId();
		if (onWindowThread && !alreadyDeferred && !IsWindowVisible(hwnd) && wxTheApp)
		{
			// called while the game window is still being created: show it once the window is up
			wxTheApp->CallAfter([message, caption, style]() { ShowOwnedMessageBox(message, caption, style, true); });
			return;
		}
		BlockUntilDocked(hwnd, onWindowThread);
		::MessageBoxW(hwnd, message.wc_str(), caption.wc_str(), MB_OK | MB_SETFOREGROUND | MessageBoxIcon(style));
	}

	// move/size hwnd to fill the client area of parent, unless it already does
	void FillParent(HWND hwnd, HWND parent)
	{
		RECT client{};
		RECT current{};
		if (!GetClientRect(parent, &client) || !GetWindowRect(hwnd, &current))
			return;
		MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<LPPOINT>(&current), 2);
		const bool fits = current.left == 0 && current.top == 0 && current.right == client.right && current.bottom == client.bottom;
		if (!fits)
			SetWindowPos(hwnd, nullptr, 0, 0, client.right, client.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	}

	// Calls tick(timer) every kPollMs on the frame's thread until tick stops the timer. onDestroy runs
	// when the frame is destroyed (the timer is stopped first).
	void Poll(wxFrame* frame, std::function<void(wxTimer&)> tick, std::function<void()> onDestroy = {})
	{
		const int id = wxWindow::NewControlId();
		auto* timer = new wxTimer(frame, id);
		frame->Bind(wxEVT_TIMER, [frame, timer, tick](wxTimerEvent&) {
			if (!frame->IsBeingDeleted())
				tick(*timer);
		}, id);
		frame->Bind(wxEVT_DESTROY, [frame, timer, onDestroy](wxWindowDestroyEvent& event) {
			if (event.GetEventObject() == frame)
			{
				timer->Stop();
				if (onDestroy)
					onDestroy();
			}
			event.Skip();
		});
		timer->Start(kPollMs);
	}
}
#endif

void RPEmbed::AttachToParent(wxFrame* frame)
{
#if BOOST_OS_WINDOWS
	const std::optional<uint64> parentId = LaunchSettings::GetRPParentWindow();
	if (!parentId || !frame)
		return;
	const HWND parent = (HWND)(uintptr_t)*parentId;
	const HWND hwnd = (HWND)frame->GetHWND();
	Remember(frame, hwnd);
	if (!IsWindow(parent))
	{
		cemuLog_log(LogType::Force, "--rp-parent: window {:#x} doesn't exist, PixelU stays in its own window", *parentId);
		return;
	}

	// borderless child window; the styles change before SetParent, as its documentation asks
	constexpr LONG_PTR kFrameStyles = WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
	constexpr LONG_PTR kFrameExStyles = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE;
	const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	SetWindowLongPtrW(hwnd, GWL_STYLE, (style & ~kFrameStyles) | WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
	const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle & ~kFrameExStyles);
	if (!SetParent(hwnd, parent))
	{
		cemuLog_log(LogType::Force, "--rp-parent: SetParent failed ({}), PixelU stays in its own window", GetLastError());
		return;
	}
	RECT client{};
	GetClientRect(parent, &client);
	SetWindowPos(hwnd, HWND_TOP, 0, 0, client.right, client.bottom, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	cemuLog_log(LogType::Force, "--rp-parent: shown inside window {:#x}", *parentId);

	// The parent belongs to another process, so its size is followed by looking at it now and then.
	// When it is gone (RetroPixels closed) PixelU closes too.
	Poll(frame, [frame, hwnd, parent](wxTimer& timer) {
		if (IsWindow(parent))
		{
			FillParent(hwnd, parent);
			return;
		}
		timer.Stop();
		frame->Close(true);
	});
#endif
}

void RPEmbed::BeforeShow(wxFrame* frame)
{
#if BOOST_OS_WINDOWS
	if (!Enabled() || !frame)
		return;
	const HWND hwnd = (HWND)frame->GetHWND();
	Remember(frame, hwnd);
	if (IsDockedWindow(hwnd))
		return; // --rp-parent already put it inside RetroPixels' window

	const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (exStyle | WS_EX_TOOLWINDOW) & ~(LONG_PTR)WS_EX_APPWINDOW);
	// Off-screen but visible at full size: RetroPixels looks for its largest visible top-level window and
	// the renderer gets a normal surface. RetroPixels' SetParent/SetWindowPos then moves it into place.
	SetWindowPos(hwnd, nullptr, kOffscreenPos, kOffscreenPos, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	s_giveUpAt = (SteadyClock::now() + kDockTimeout).time_since_epoch().count();

	Poll(frame,
		 [hwnd](wxTimer& timer) {
			 if (IsDockedWindow(hwnd))
			 {
				 timer.Stop();
				 cemuLog_log(LogType::Force, "--rp-embed: docked into window {:#x}", (uintptr_t)HostOf(hwnd));
			 }
			 else if (WaitTimedOut())
			 {
				 timer.Stop();
				 BecomeStandalone();
			 }
		 },
		 []() { Remember(nullptr, nullptr); });
#endif
}

bool RPEmbed::IsDocked()
{
#if BOOST_OS_WINDOWS
	if (!Enabled())
		return false;
	const HWND hwnd = s_mainHwnd.load();
	return hwnd && IsDockedWindow(hwnd);
#else
	return false;
#endif
}

void RPEmbed::ShowMessage(const wxString& message, const wxString& caption, long style)
{
	if (!Enabled())
	{
		wxMessageBox(message, caption, style);
		return;
	}
	cemuLog_log(LogType::Force, "{}: {}", caption.utf8_string(), message.utf8_string());
#if BOOST_OS_WINDOWS
	ShowOwnedMessageBox(message, caption, style, false);
#else
	wxMessageBox(message, caption, style);
#endif
}

namespace
{
	// wxWidgets log target that writes to PixelU's log instead of showing anything
	class LogToFile : public wxLog
	{
	  protected:
		void DoLogTextAtLevel(wxLogLevel level, const wxString& msg) override
		{
			cemuLog_log(LogType::Force, "wxWidgets: {}", msg.utf8_string());
		}
	};
}

void RPEmbed::RouteWxLog()
{
	if (!Enabled())
		return;
	delete wxLog::SetActiveTarget(new LogToFile());
}
