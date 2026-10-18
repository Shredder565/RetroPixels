// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "wxgui/wxgui.h"
#include "wxgui/RPEmbed.h"
#include "config/LaunchSettings.h"

bool RPEmbed::Enabled()
{
	return LaunchSettings::RPEmbedEnabled();
}

long RPEmbed::FrameStyle(long defaultStyle)
{
	if (!Enabled())
		return defaultStyle;
	// a plain resizable window: no minimize/maximize buttons, nothing else in the title bar
	return wxCAPTION | wxSYSTEM_MENU | wxCLOSE_BOX | wxRESIZE_BORDER | wxCLIP_CHILDREN;
}

#if BOOST_OS_WINDOWS
namespace
{
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
