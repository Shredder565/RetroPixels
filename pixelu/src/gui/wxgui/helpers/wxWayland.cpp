// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/helpers/wxWayland.h"

#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND

#include <dlfcn.h>

bool wxWlIsWaylandWindow(wxWindow* window)
{
	auto* widget = static_cast<GtkWidget*>(window->GetHandle());
	return GDK_IS_WAYLAND_WINDOW(gtk_widget_get_window(widget));
}

void wxWlSetAppId(wxFrame* frame, const char* applicationId)
{
	auto* widget = static_cast<GtkWidget*>(frame->GetHandle());
	gtk_widget_realize(widget);
	// only newer GTK versions export this, so look it up at run time
	using SetAppIdFn = void (*)(GdkWindow*, const char*);
	static const SetAppIdFn setAppId = reinterpret_cast<SetAppIdFn>(dlsym(nullptr, "gdk_wayland_window_set_application_id"));
	if (setAppId != nullptr)
		setAppId(gtk_widget_get_window(widget), applicationId);
}

#endif // ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
