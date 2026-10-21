// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/RPEmbed.h"
#include "config/LaunchSettings.h"

#include <wx/log.h>
#include <wx/msgdlg.h>

#include <atomic>
#include <chrono>
#include <functional>

#if BOOST_OS_LINUX || BOOST_OS_BSD
#include "wxgui/MainWindow.h"
#include "wxgui/RPEmbedX11.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"

#include <thread>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#endif

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

#if BOOST_OS_LINUX || BOOST_OS_BSD
namespace
{
	using SteadyClock = std::chrono::steady_clock;
	using XWindow = RPEmbedX11::WindowId;

	constexpr auto kDockTimeout = std::chrono::seconds(20);
	constexpr int kPollMs = 100;

	wxFrame* s_mainFrame = nullptr;
	// the main frame's X11 window; 0 before BeforeShow/AttachToParent or when the GUI doesn't run on X11
	XWindow s_window = 0;
	// what it is docked into: the --rp-parent window, the WM_TRANSIENT_FOR window or the window it was
	// reparented into; 0 when it was docked only by being mapped, or isn't docked
	XWindow s_host = 0;
	// --rp-parent: the frame keeps filling s_host
	bool s_fillHost = false;
	bool s_x11Ready = false;
	std::atomic_bool s_docked{false};
	std::atomic_bool s_standalone{false};
	SteadyClock::time_point s_giveUpAt{};

	GtkWindow* GtkTop(wxFrame* frame)
	{
		return GTK_WINDOW(static_cast<GtkWidget*>(frame->GetHandle()));
	}

	// "PixelU.Screen" client messages; RetroPixels sends them to the game window
	GdkFilterReturn OnXEvent(GdkXEvent* gdkXEvent, GdkEvent*, gpointer)
	{
		const XEvent* event = static_cast<const XEvent*>(gdkXEvent);
		constexpr int kClientMessage = 33; // X11's ClientMessage, whose macro platform.h undefines
		if (event->type != kClientMessage)
			return GDK_FILTER_CONTINUE;
		const XClientMessageEvent& message = event->xclient;
		const std::optional<RPEmbedX11::ScreenRequest> request =
			RPEmbedX11::ParseScreenMessage((RPEmbedX11::AtomId)message.message_type, message.format, message.data.l);
		if (!request)
			return GDK_FILTER_CONTINUE;
		const bool gamePadShown = MainWindow::SetScreenMode(request->mode);
		RPEmbedX11::PublishScreen((XWindow)message.window, request->requestNumber, gamePadShown);
		return GDK_FILTER_REMOVE;
	}

	// Creates the frame's X11 window without showing it, so RetroPixels can find it (GDK sets _NET_WM_PID)
	// and dock it, marks it as the game window and starts answering screen requests. 0 when the GUI
	// doesn't run on X11.
	XWindow RealizeOnX11(wxFrame* frame)
	{
		if (s_window)
			return s_window;
		GtkWidget* top = static_cast<GtkWidget*>(frame->GetHandle());
		gtk_widget_realize(top);
		GdkWindow* gdkWindow = gtk_widget_get_window(top);
		if (!gdkWindow || !GDK_IS_X11_WINDOW(gdkWindow))
			return 0;
		if (!s_x11Ready)
		{
			Display* display = gdk_x11_display_get_xdisplay(gdk_window_get_display(gdkWindow));
			s_x11Ready = display && RPEmbedX11::Init(XGetXCBConnection(display));
			if (!s_x11Ready)
				return 0;
			gdk_window_add_filter(nullptr, OnXEvent, nullptr);
		}
		s_window = (XWindow)gdk_x11_window_get_xid(gdkWindow);
		RPEmbedX11::PublishScreen(s_window, 0, MainWindow::SetScreenMode(-1));
		if (LaunchSettings::RPEmbedEnabled())
			RPEmbedX11::SetEmbedMark(s_window);
		return s_window;
	}

	bool WaitTimedOut()
	{
		return SteadyClock::now() >= s_giveUpAt;
	}

	// Shows the frame without a frame of its own, where RetroPixels put it. GTK applies its own size and
	// place when it maps a window, so wx first learns the geometry RetroPixels gave the X window.
	void ShowDocked(wxFrame* frame, XWindow host)
	{
		s_host = host;
		s_docked = true;
		// put into one of RetroPixels' windows: fill it (RetroPixels may size it after it reparents)
		const std::optional<XWindow> parent = RPEmbedX11::ParentOf(s_window);
		s_fillHost = host != 0 && parent && *parent == host;
		const auto area = s_fillHost ? RPEmbedX11::GetGeometry(host) : std::nullopt;
		if (area)
			frame->SetSize(0, 0, (int)area->width, (int)area->height);
		else if (const auto geometry = RPEmbedX11::GetGeometry(s_window))
			frame->SetSize(geometry->x, geometry->y, (int)geometry->width, (int)geometry->height);
		GtkWindow* top = GtkTop(frame);
		gtk_window_set_decorated(top, FALSE);
		gtk_window_set_skip_taskbar_hint(top, TRUE);
		gtk_window_set_skip_pager_hint(top, TRUE);
		frame->Show();
		RPEmbedX11::Flush();
		cemuLog_log(LogType::Force, "--rp-embed: docked into window {:#x}", host);
	}

	// Nobody docked it in time: make it the window it would be without --rp-embed. Only once.
	void BecomeStandalone(wxFrame* frame)
	{
		if (!frame || frame->IsBeingDeleted() || s_docked || s_standalone.exchange(true))
			return;
		cemuLog_log(LogType::Force, "--rp-embed: the window wasn't docked within {} seconds, showing it on its own", (int)kDockTimeout.count());
		GtkWindow* top = GtkTop(frame);
		gtk_window_set_skip_taskbar_hint(top, FALSE);
		gtk_window_set_skip_pager_hint(top, FALSE);
		gtk_window_set_focus_on_map(top, TRUE);
		frame->Centre();
		frame->Show();
		frame->Raise();
	}

	// One look at the window: is it docked now? Shows it when it is, or on its own when the wait is
	// over (it can still be docked later). True once it is shown either way.
	bool CheckDocking(wxFrame* frame)
	{
		if (s_docked)
			return true;
		const bool waiting = !s_standalone;
		const XWindow host = RPEmbedX11::FindHost(s_window, waiting);
		if (host != 0 || (waiting && RPEmbedX11::IsMapped(s_window)))
		{
			ShowDocked(frame, host);
			return true;
		}
		if (waiting && WaitTimedOut())
			BecomeStandalone(frame);
		return s_standalone;
	}

	// keeps the frame filling parent at 0,0 (--rp-parent)
	void FillParent(XWindow parent)
	{
		const auto area = RPEmbedX11::GetGeometry(parent);
		const auto current = RPEmbedX11::GetGeometry(s_window);
		if (!area || !current)
			return;
		const RPEmbedX11::Geometry wanted{0, 0, area->width, area->height};
		if (!(*current == wanted))
			RPEmbedX11::MoveResize(s_window, 0, 0, area->width, area->height);
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

	// Waits for docking, then while docked: follows the --rp-parent window's size, and closes PixelU
	// when the window it is docked into is gone (RetroPixels closed).
	void WatchDocking(wxFrame* frame)
	{
		Poll(frame,
			 [frame](wxTimer& timer) {
				 if (!CheckDocking(frame) || !s_docked)
					 return; // waiting, or shown on its own (RetroPixels can still dock it)
				 if (s_host == 0)
				 {
					 timer.Stop();
					 return;
				 }
				 if (RPEmbedX11::WindowExists(s_host))
				 {
					 if (s_fillHost)
						 FillParent(s_host);
					 return;
				 }
				 timer.Stop();
				 cemuLog_log(LogType::Force, "--rp-embed: window {:#x} it was docked into is gone, closing", s_host);
				 frame->Close(true);
			 },
			 []() {
				 s_mainFrame = nullptr;
				 s_window = 0;
				 s_host = 0;
			 });
	}

	// SIGTERM (RetroPixels' Stop, after asking the window to close) closes the frame like its close
	// button, so a recording is finished; a second one still ends PixelU at once
	void CloseOnStopSignal(wxFrame* frame)
	{
		ExceptionHandler_DeferStopSignal();
		Poll(frame, [frame](wxTimer& timer) {
			if (!ExceptionHandler_StopRequested())
				return;
			timer.Stop();
			cemuLog_log(LogType::Force, "Stop signal: closing the window");
			frame->Close(true);
		});
	}

	long DialogIcon(long style)
	{
		if (style & wxICON_INFORMATION)
			return wxICON_INFORMATION;
		if (style & (wxICON_WARNING | wxICON_EXCLAMATION))
			return wxICON_WARNING;
		return wxICON_ERROR;
	}

	// Under --rp-embed, on the GUI thread: the box belongs to the game window. While that waits hidden
	// to be docked the box waits too (looking at the X server needs no event loop), so it is never seen
	// on its own. Before the game window exists the log line is all there is.
	void ShowEmbeddedMessage(const wxString& message, const wxString& caption, long style)
	{
		wxFrame* frame = s_mainFrame;
		if (!frame || frame->IsBeingDeleted())
			return;
		if (s_window && !frame->IsShown())
		{
			while (!CheckDocking(frame))
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		if (!frame->IsShown())
			return;
		wxMessageDialog dialog(frame, message, caption, wxOK | wxCENTRE | DialogIcon(style));
		dialog.ShowModal();
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
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	const std::optional<uint64> parentId = LaunchSettings::GetRPParentWindow();
	if (!parentId || !frame)
		return;
	s_mainFrame = frame;
	const XWindow parent = (XWindow)*parentId;
	if (!RealizeOnX11(frame))
	{
		cemuLog_log(LogType::Force, "--rp-parent: the window isn't an X11 window (Wayland?), PixelU stays in its own window");
		return;
	}
	if (*parentId > 0xFFFFFFFFull || !RPEmbedX11::WindowExists(parent))
	{
		cemuLog_log(LogType::Force, "--rp-parent: window {:#x} doesn't exist, PixelU stays in its own window", *parentId);
		return;
	}
	gtk_window_set_decorated(GtkTop(frame), FALSE);
	gtk_window_set_skip_taskbar_hint(GtkTop(frame), TRUE);
	gtk_window_set_skip_pager_hint(GtkTop(frame), TRUE);
	if (!RPEmbedX11::Reparent(s_window, parent))
	{
		cemuLog_log(LogType::Force, "--rp-parent: reparenting failed, PixelU stays in its own window");
		return;
	}
	if (const auto area = RPEmbedX11::GetGeometry(parent))
	{
		RPEmbedX11::MoveResize(s_window, 0, 0, area->width, area->height);
		frame->SetSize(0, 0, (int)area->width, (int)area->height);
	}
	s_host = parent;
	s_fillHost = true;
	s_docked = true;
	cemuLog_log(LogType::Force, "--rp-parent: shown inside window {:#x}", *parentId);
	// the parent belongs to another process: its size is followed by looking at it now and then, and
	// PixelU closes when it is gone (RetroPixels closed)
	WatchDocking(frame);
#endif
}

bool RPEmbed::BeforeShow(wxFrame* frame)
{
#if BOOST_OS_WINDOWS
	if (!Enabled() || !frame)
		return true;
	const HWND hwnd = (HWND)frame->GetHWND();
	Remember(frame, hwnd);
	if (IsDockedWindow(hwnd))
		return true; // --rp-parent already put it inside RetroPixels' window

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
	return true;
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	if (!frame)
		return true;
	if (Enabled() || LatteRecorder::IsEnabled())
		CloseOnStopSignal(frame);
	if (!Enabled())
		return true;
	s_mainFrame = frame;
	if (s_docked)
		return true; // --rp-parent already put it inside RetroPixels' window
	if (!RealizeOnX11(frame))
	{
		cemuLog_log(LogType::Force, "--rp-embed: the window isn't an X11 window (Wayland?), it can't be docked and is shown on its own");
		s_standalone = true;
		return true;
	}
	// hidden, and without a task bar or pager entry once shown docked
	GtkWindow* top = GtkTop(frame);
	gtk_window_set_skip_taskbar_hint(top, TRUE);
	gtk_window_set_skip_pager_hint(top, TRUE);
	gtk_window_set_focus_on_map(top, FALSE);
	s_giveUpAt = SteadyClock::now() + kDockTimeout;
	cemuLog_log(LogType::Force, "--rp-embed: window {:#x} waits to be docked", s_window);
	WatchDocking(frame);
	return false;
#else
	return true;
#endif
}

bool RPEmbed::IsDocked()
{
#if BOOST_OS_WINDOWS
	if (!Enabled())
		return false;
	const HWND hwnd = s_mainHwnd.load();
	return hwnd && IsDockedWindow(hwnd);
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	return Enabled() && s_docked;
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
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	if (!wxTheApp)
		return;
	if (wxIsMainThread())
		ShowEmbeddedMessage(message, caption, style);
	else
		wxTheApp->CallAfter([message, caption, style]() { ShowEmbeddedMessage(message, caption, style); });
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
