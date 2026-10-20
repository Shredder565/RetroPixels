// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/helpers/wxHelpers.h"

#include <wx/wupdlock.h>
#include <wx/stattext.h>
#include <wx/slider.h>
#include <wx/dirdlg.h>

#if BOOST_OS_LINUX || BOOST_OS_BSD
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkwindow.h>
#include <gdk/gdkx.h>
#ifdef HAS_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#endif

#include "wxgui/helpers/wxControlObject.h"

void wxAutosizeColumn(wxListCtrlBase* ctrl, int col)
{
	// size to the header first, then to the content; keep whichever is wider
	ctrl->SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
	const int headerWidth = ctrl->GetColumnWidth(col);
	ctrl->SetColumnWidth(col, wxLIST_AUTOSIZE);
	if (headerWidth > ctrl->GetColumnWidth(col))
		ctrl->SetColumnWidth(col, wxLIST_AUTOSIZE_USEHEADER);
}

void wxAutosizeColumns(wxListCtrlBase* ctrl, int col_start, int col_end)
{
	wxWindowUpdateLocker freeze(ctrl);
	for (int c = col_start; c <= col_end; c++)
		wxAutosizeColumn(ctrl, c);
}

void update_slider_text(wxCommandEvent& event, const wxFormatString& format)
{
	auto* source = dynamic_cast<wxSlider*>(event.GetEventObject());
	wxASSERT(source);
	auto* label = dynamic_cast<wxControlObject*>(event.GetEventUserData())->GetControl<wxStaticText>();
	wxASSERT(label);
	label->SetLabel(wxString::Format(format, source->GetValue()));
}

uint32 fix_raw_keycode(uint32 keycode, uint32 raw_flags)
{
#if BOOST_OS_WINDOWS
	// bits 16..27 of the raw flags hold the scan code plus the extended-key bit
	const uint32 scan = (raw_flags >> 16) & 0xFFF;
	switch (keycode)
	{
	case VK_SHIFT:
		if (scan == 0x2A)
			return VK_LSHIFT;
		if (scan == 0x36)
			return VK_RSHIFT;
		break;
	case VK_CONTROL:
		if (scan == 0x01D)
			return VK_LCONTROL;
		if (scan == 0x11D)
			return VK_RCONTROL;
		break;
	case VK_MENU:
		// both alt keys report scan code 0x38 here and have always been stored as the left one
		if ((scan & 0xFF) == 0x38)
			return VK_LMENU;
		break;
	default:
		break;
	}
#endif
	return keycode;
}

WindowSystem::WindowHandleInfo initHandleContextFromWxWidgetsWindow(wxWindow* wxw)
{
	WindowSystem::WindowHandleInfo info;
#if BOOST_OS_WINDOWS
	info.backend = WindowSystem::WindowHandleInfo::Backend::Windows;
	info.surface = reinterpret_cast<void*>(wxw->GetHWND());
#elif BOOST_OS_LINUX || BOOST_OS_BSD
	auto* widget = static_cast<GtkWidget*>(wxw->GetHandle());
	gtk_widget_realize(widget);
	GdkWindow* gdkWin = gtk_widget_get_window(widget);
	GdkDisplay* gdkDisp = gdk_window_get_display(gdkWin);
	if (GDK_IS_X11_WINDOW(gdkWin))
	{
		info.backend = WindowSystem::WindowHandleInfo::Backend::X11;
		info.surface = reinterpret_cast<void*>(gdk_x11_window_get_xid(gdkWin));
		info.display = gdk_x11_display_get_xdisplay(gdkDisp);
		if (info.display == nullptr)
			cemuLog_log(LogType::Force, "Unable to get xlib display");
	}
#ifdef HAS_WAYLAND
	else if (GDK_IS_WAYLAND_WINDOW(gdkWin))
	{
		info.backend = WindowSystem::WindowHandleInfo::Backend::Wayland;
		info.surface = gdk_wayland_window_get_wl_surface(gdkWin);
		info.display = gdk_wayland_display_get_wl_display(gdkDisp);
	}
#endif
	else
	{
		cemuLog_log(LogType::Force, "Unsupported GTK backend");
	}
#elif BOOST_OS_MACOS
	info.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
	info.surface = reinterpret_cast<void*>(wxw->GetHandle());
#endif
	return info;
}
