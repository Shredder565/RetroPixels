// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#if ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND

#include <gdk/gdk.h>
#include <gdk/gdkwayland.h>
#include <gtk/gtk.h>
#include <wayland-client.h>
#include <wx/wx.h>
#include "wayland-viewporter-client-protocol.h"

// A desynchronised Wayland subsurface placed over a wx window, used as the
// Vulkan render target on Wayland. It ignores input and is scaled to the window
// through wp_viewporter when the compositor offers it.
class wxWlSubsurface
{
public:
	wxWlSubsurface(wxWindow* window)
	{
		auto* widget = static_cast<GtkWidget*>(window->GetHandle());
		gtk_widget_realize(widget);
		GdkWindow* gdkWin = gtk_widget_get_window(widget);
		GdkDisplay* gdkDisp = gdk_window_get_display(gdkWin);
		wl_display* display = gdk_wayland_display_get_wl_display(gdkDisp);
		wl_surface* parentSurface = gdk_wayland_window_get_wl_surface(gdkWin);
		wl_compositor* compositor = gdk_wayland_display_get_wl_compositor(gdkDisp);

		// look up the subcompositor and viewporter globals
		wl_registry* registry = wl_display_get_registry(display);
		wl_registry_add_listener(registry, &m_listener, this);
		wl_display_roundtrip(display);

		m_surface = wl_compositor_create_surface(compositor);
		if (m_viewporter != nullptr)
			m_viewport = wp_viewporter_get_viewport(m_viewporter, m_surface);

		wl_region* noInput = wl_compositor_create_region(compositor);
		wl_surface_set_input_region(m_surface, noInput);
		m_subsurface = wl_subcompositor_get_subsurface(m_subcompositor, m_surface, parentSurface);
		wl_subsurface_set_desync(m_subsurface);
		const wxRect r = window->GetScreenRect();
		setSize(r.x, r.y, r.width, r.height);
		wl_region_destroy(noInput);
	}

	~wxWlSubsurface()
	{
		wl_subsurface_destroy(m_subsurface);
		wl_surface_destroy(m_surface);
		wl_subcompositor_destroy(m_subcompositor);
	}

	wl_surface* getSurface() const { return m_surface; }

	void setSize(int32_t xPos, int32_t yPos, int32_t width, int32_t height)
	{
		if (xPos == m_x && yPos == m_y && width == m_w && height == m_h)
			return;
		m_x = xPos;
		m_y = yPos;
		m_w = width;
		m_h = height;
		wl_subsurface_set_position(m_subsurface, m_x, m_y);
		if (m_viewport != nullptr)
			wp_viewport_set_destination(m_viewport, m_w, m_h);
		wl_surface_commit(m_surface);
	}

private:
	static void OnGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t /*version*/)
	{
		auto* self = static_cast<wxWlSubsurface*>(data);
		if (strcmp(interface, wl_subcompositor_interface.name) == 0)
			self->m_subcompositor = static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
		else if (strcmp(interface, wp_viewporter_interface.name) == 0)
			self->m_viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
	}
	static void OnGlobalRemove(void*, wl_registry*, uint32_t) {}

	wl_registry_listener m_listener = {&OnGlobal, &OnGlobalRemove};
	wl_subcompositor* m_subcompositor = nullptr;
	wl_surface* m_surface = nullptr;
	wl_subsurface* m_subsurface = nullptr;
	wp_viewporter* m_viewporter = nullptr;
	wp_viewport* m_viewport = nullptr;
	int32_t m_x = 0;
	int32_t m_y = 0;
	int32_t m_w = 0;
	int32_t m_h = 0;
};

bool wxWlIsWaylandWindow(wxWindow* window);
void wxWlSetAppId(wxFrame* frame, const char* application_id);

#endif // ( BOOST_OS_LINUX || BOOST_OS_BSD ) && HAS_WAYLAND
