// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "wxgui/wxgui.h"
#include "wxgui/RPEmbed.h"
#include "config/LaunchSettings.h"

#include <wx/log.h>
#include <wx/msgdlg.h>

#include <atomic>
#include <chrono>

bool RPEmbed::Enabled()
{
	return LaunchSettings::RPEmbedEnabled();
}

long RPEmbed::FrameStyle(long defaultStyle)
{
	if (!Enabled())
		return defaultStyle;
	// a plain resizable window: no minimize/maximize buttons, nothing else in the title bar
	long style = wxCAPTION | wxSYSTEM_MENU | wxCLOSE_BOX | wxRESIZE_BORDER | wxCLIP_CHILDREN;
#if BOOST_OS_WINDOWS
	// WS_EX_TOOLWINDOW: no taskbar button and not in Alt+Tab while it waits to be docked
	// (it also makes wxWidgets show it without activating it)
	style |= wxFRAME_TOOL_WINDOW;
#endif
	return style;
}

#if BOOST_OS_WINDOWS
namespace
{
	constexpr int kOffscreen = -32000;
	constexpr auto kDockWait = std::chrono::seconds(20);

	std::atomic<HWND> s_hwnd{nullptr}; // the main frame
	wxFrame* s_frame = nullptr;
	std::atomic<std::chrono::steady_clock::time_point::rep> s_dockDeadline{0};
	std::atomic_bool s_onScreen{false}; // not docked in time, shown as a normal window

	bool IsChildWindow(HWND hwnd)
	{
		return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) != 0 || GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow();
	}

	bool DockDeadlinePassed()
	{
		return std::chrono::steady_clock::now().time_since_epoch().count() >= s_dockDeadline.load();
	}

	// nobody docked it: show it as the normal window it would be without --rp-embed, on screen and in the taskbar
	void ShowOnScreen()
	{
		wxFrame* frame = s_frame;
		const HWND hwnd = s_hwnd.load();
		if (!frame || !hwnd || frame->IsBeingDeleted() || IsChildWindow(hwnd) || s_onScreen.exchange(true))
			return;
		cemuLog_log(LogType::Force, "--rp-embed: the window wasn't docked within {} seconds, showing it on its own", (int)kDockWait.count());
		frame->Hide();
		frame->SetWindowStyleFlag(frame->GetWindowStyleFlag() & ~wxFRAME_TOOL_WINDOW);
		frame->Centre();
		frame->Show();
		frame->Raise();
	}

	// wait until the game window is docked (or shown on its own instead). On the window's own thread
	// the messages SetParent/SetWindowPos send from RetroPixels are answered meanwhile (only those:
	// no wx events run in between).
	void WaitForDock(HWND hwnd, bool ownThread)
	{
		while (IsWindow(hwnd) && !IsChildWindow(hwnd) && !s_onScreen && !DockDeadlinePassed())
		{
			if (ownThread)
			{
				MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_SENDMESSAGE);
				MSG msg;
				PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
			}
			else
				Sleep(20);
		}
		if (ownThread && IsWindow(hwnd) && !IsChildWindow(hwnd))
			ShowOnScreen();
	}

	void ShowMessageEmbedded(const wxString& message, const wxString& caption, long style, bool deferred)
	{
		const HWND hwnd = s_hwnd.load();
		if (!hwnd || !IsWindow(hwnd))
			return; // no game window (yet): only the log, PixelU doesn't show a window by itself
		const bool ownThread = GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId();
		if (ownThread && !IsWindowVisible(hwnd) && !deferred && wxTheApp)
		{
			// the game window isn't shown yet (called while it is made): once it is
			wxTheApp->CallAfter([message, caption, style]() { ShowMessageEmbedded(message, caption, style, true); });
			return;
		}
		WaitForDock(hwnd, ownThread);
		UINT type = MB_OK | MB_SETFOREGROUND;
		if (style & wxICON_INFORMATION)
			type |= MB_ICONINFORMATION;
		else if (style & wxICON_WARNING)
			type |= MB_ICONWARNING;
		else
			type |= MB_ICONERROR;
		::MessageBoxW(hwnd, message.wc_str(), caption.wc_str(), type);
	}

	void FitToParent(HWND hwnd, HWND parent)
	{
		RECT parentRect{}, ownRect{};
		if (!GetClientRect(parent, &parentRect) || !GetWindowRect(hwnd, &ownRect))
			return;
		MapWindowPoints(HWND_DESKTOP, parent, (LPPOINT)&ownRect, 2);
		if (ownRect.left == 0 && ownRect.top == 0 && ownRect.right == parentRect.right && ownRect.bottom == parentRect.bottom)
			return;
		SetWindowPos(hwnd, nullptr, 0, 0, parentRect.right, parentRect.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	}
}
#endif

void RPEmbed::AttachToParent(wxFrame* frame)
{
#if BOOST_OS_WINDOWS
	const auto parentValue = LaunchSettings::GetRPParentWindow();
	if (!parentValue || !frame)
		return;
	const HWND parent = (HWND)(uintptr_t)*parentValue;
	const HWND hwnd = (HWND)frame->GetHWND();
	s_frame = frame;
	s_hwnd = hwnd;
	if (!IsWindow(parent))
	{
		cemuLog_log(LogType::Force, "--rp-parent: window {:#x} doesn't exist, PixelU stays in its own window", *parentValue);
		return;
	}

	// a borderless child window (style changes before SetParent, as the SetParent docs ask)
	LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	style &= ~(LONG_PTR)(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
	style |= WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
	SetWindowLongPtrW(hwnd, GWL_STYLE, style);
	LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	exStyle &= ~(LONG_PTR)(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
	if (!SetParent(hwnd, parent))
	{
		cemuLog_log(LogType::Force, "--rp-parent: SetParent failed ({}), PixelU stays in its own window", GetLastError());
		return;
	}
	RECT parentRect{};
	GetClientRect(parent, &parentRect);
	SetWindowPos(hwnd, HWND_TOP, 0, 0, parentRect.right, parentRect.bottom, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	cemuLog_log(LogType::Force, "--rp-parent: shown inside window {:#x}", *parentValue);

	// the parent belongs to another process, so follow its size by looking at it now and then;
	// when it is gone (RetroPixels closed) PixelU closes too
	const int timerId = wxWindow::NewControlId();
	auto* timer = new wxTimer(frame, timerId);
	frame->Bind(wxEVT_TIMER, [frame, timer, hwnd, parent](wxTimerEvent&)
	{
		if (frame->IsBeingDeleted())
			return;
		if (!IsWindow(parent))
		{
			timer->Stop();
			frame->Close(true);
			return;
		}
		FitToParent(hwnd, parent);
	}, timerId);
	frame->Bind(wxEVT_DESTROY, [frame, timer](wxWindowDestroyEvent& event)
	{
		if (event.GetEventObject() == frame)
			timer->Stop();
		event.Skip();
	});
	timer->Start(100);
#endif
}

void RPEmbed::BeforeShow(wxFrame* frame)
{
#if BOOST_OS_WINDOWS
	if (!Enabled() || !frame)
		return;
	const HWND hwnd = (HWND)frame->GetHWND();
	s_frame = frame;
	s_hwnd = hwnd;
	if (IsChildWindow(hwnd))
		return; // --rp-parent: already inside RetroPixels' window
	LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (exStyle | WS_EX_TOOLWINDOW) & ~(LONG_PTR)WS_EX_APPWINDOW);
	// off-screen but "visible" at its full size, so RetroPixels finds it (its largest visible top-level
	// window) and the renderer gets a normal surface; RetroPixels' SetParent/SetWindowPos then moves it in
	SetWindowPos(hwnd, nullptr, kOffscreen, kOffscreen, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	s_dockDeadline = (std::chrono::steady_clock::now() + kDockWait).time_since_epoch().count();

	const int timerId = wxWindow::NewControlId();
	auto* timer = new wxTimer(frame, timerId);
	frame->Bind(wxEVT_TIMER, [frame, timer, hwnd](wxTimerEvent&)
	{
		if (frame->IsBeingDeleted())
			return;
		if (IsChildWindow(hwnd))
		{
			timer->Stop();
			cemuLog_log(LogType::Force, "--rp-embed: docked into window {:#x}", (uintptr_t)GetAncestor(hwnd, GA_PARENT));
			return;
		}
		if (DockDeadlinePassed())
		{
			timer->Stop();
			ShowOnScreen();
		}
	}, timerId);
	frame->Bind(wxEVT_DESTROY, [frame, timer](wxWindowDestroyEvent& event)
	{
		if (event.GetEventObject() == frame)
		{
			timer->Stop();
			s_frame = nullptr;
			s_hwnd = nullptr;
		}
		event.Skip();
	});
	timer->Start(100);
#endif
}

bool RPEmbed::IsDocked()
{
#if BOOST_OS_WINDOWS
	const HWND hwnd = s_hwnd.load();
	return Enabled() && hwnd && IsChildWindow(hwnd);
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
	ShowMessageEmbedded(message, caption, style, false);
#else
	wxMessageBox(message, caption, style);
#endif
}

namespace
{
	class RPEmbedLog : public wxLog
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
	if (Enabled())
		delete wxLog::SetActiveTarget(new RPEmbedLog());
}
