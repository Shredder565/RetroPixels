// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/RPEmbedX11.h"

#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>

namespace
{
	xcb_connection_t* s_conn = nullptr;
	xcb_atom_t s_screenAtom = XCB_ATOM_NONE;
	xcb_atom_t s_embedAtom = XCB_ATOM_NONE; // _RETROPIXELS_EMBED, on a window waiting to be docked
	xcb_atom_t s_dockAtom = XCB_ATOM_NONE; // _RETROPIXELS_DOCK, on RetroPixels' window it is docked into
	uint32_t s_idBase = 0;
	uint32_t s_idMask = 0;

	// every reply below is freed by this
	template<typename T>
	struct Reply
	{
		T* p = nullptr;
		explicit Reply(T* reply) : p(reply) {}
		~Reply() { free(p); }
		Reply(const Reply&) = delete;
		Reply& operator=(const Reply&) = delete;
		T* operator->() const { return p; }
		explicit operator bool() const { return p != nullptr; }
	};

	// a reply that comes back as an error (a window that is gone) is no reply; the error is dropped
	template<typename T>
	T* Checked(T* reply, xcb_generic_error_t* error)
	{
		free(error);
		return reply;
	}

	xcb_atom_t Intern(const char* name)
	{
		xcb_generic_error_t* error = nullptr;
		Reply r(Checked(xcb_intern_atom_reply(s_conn, xcb_intern_atom(s_conn, 0, (uint16_t)strlen(name), name), &error), error));
		return r ? r->atom : XCB_ATOM_NONE;
	}

	std::optional<RPEmbedX11::WindowId> TransientFor(RPEmbedX11::WindowId window)
	{
		xcb_generic_error_t* error = nullptr;
		const auto cookie = xcb_get_property(s_conn, 0, window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 1);
		Reply r(Checked(xcb_get_property_reply(s_conn, cookie, &error), error));
		if (!r || r->format != 32 || xcb_get_property_value_length(r.p) < 4)
			return std::nullopt;
		return *static_cast<const uint32_t*>(xcb_get_property_value(r.p));
	}

	bool IsRoot(RPEmbedX11::WindowId window)
	{
		for (auto it = xcb_setup_roots_iterator(xcb_get_setup(s_conn)); it.rem; xcb_screen_next(&it))
			if (it.data->root == window)
				return true;
		return false;
	}

	// RetroPixels' box: _RETROPIXELS_DOCK is set (to RetroPixels' pid)
	bool HasDockMark(RPEmbedX11::WindowId window)
	{
		if (s_dockAtom == XCB_ATOM_NONE)
			return false;
		xcb_generic_error_t* error = nullptr;
		const auto cookie = xcb_get_property(s_conn, 0, window, s_dockAtom, XCB_ATOM_CARDINAL, 0, 1);
		Reply r(Checked(xcb_get_property_reply(s_conn, cookie, &error), error));
		return r && r->format == 32 && xcb_get_property_value_length(r.p) >= 4;
	}
}

namespace RPEmbedX11
{
	bool Init(xcb_connection_t* connection)
	{
		s_conn = connection;
		if (!s_conn || xcb_connection_has_error(s_conn))
			return false;
		const xcb_setup_t* setup = xcb_get_setup(s_conn);
		s_idBase = setup->resource_id_base;
		s_idMask = setup->resource_id_mask;
		s_screenAtom = Intern(kScreenName);
		s_embedAtom = Intern("_RETROPIXELS_EMBED");
		s_dockAtom = Intern("_RETROPIXELS_DOCK");
		return s_screenAtom != XCB_ATOM_NONE;
	}

	AtomId ScreenAtom()
	{
		return s_screenAtom;
	}

	void PublishScreen(WindowId window, uint32_t requestNumber, bool gamePadShown)
	{
		if (!s_conn || s_screenAtom == XCB_ATOM_NONE)
			return;
		const uint32_t value[2] = {requestNumber, gamePadShown ? 1u : 0u};
		xcb_change_property(s_conn, XCB_PROP_MODE_REPLACE, window, s_screenAtom, XCB_ATOM_CARDINAL, 32, 2, value);
		xcb_flush(s_conn);
	}

	std::optional<ScreenRequest> ParseScreenMessage(AtomId type, int format, const long data[5])
	{
		if (s_screenAtom == XCB_ATOM_NONE || type != s_screenAtom || format != 32)
			return std::nullopt;
		// Xlib hands format-32 data out as longs; only the low 32 bits were sent
		return ScreenRequest{(int)(int32_t)(uint32_t)data[0], (uint32_t)data[1]};
	}

	bool IsOwnWindow(WindowId window)
	{
		return window != 0 && (window & ~s_idMask) == s_idBase;
	}

	bool WindowExists(WindowId window)
	{
		if (!s_conn || window == 0)
			return false;
		xcb_generic_error_t* error = nullptr;
		Reply r(Checked(xcb_get_window_attributes_reply(s_conn, xcb_get_window_attributes(s_conn, window), &error), error));
		return (bool)r;
	}

	bool IsMapped(WindowId window)
	{
		if (!s_conn || window == 0)
			return false;
		xcb_generic_error_t* error = nullptr;
		Reply r(Checked(xcb_get_window_attributes_reply(s_conn, xcb_get_window_attributes(s_conn, window), &error), error));
		return r && r->map_state != XCB_MAP_STATE_UNMAPPED;
	}

	std::optional<WindowId> ParentOf(WindowId window)
	{
		if (!s_conn || window == 0)
			return std::nullopt;
		xcb_generic_error_t* error = nullptr;
		Reply r(Checked(xcb_query_tree_reply(s_conn, xcb_query_tree(s_conn, window), &error), error));
		if (!r)
			return std::nullopt;
		return r->parent;
	}

	std::optional<Geometry> GetGeometry(WindowId window)
	{
		if (!s_conn || window == 0)
			return std::nullopt;
		xcb_generic_error_t* error = nullptr;
		Reply r(Checked(xcb_get_geometry_reply(s_conn, xcb_get_geometry(s_conn, window), &error), error));
		if (!r)
			return std::nullopt;
		return Geometry{r->x, r->y, r->width, r->height};
	}

	void MoveResize(WindowId window, int x, int y, uint32_t width, uint32_t height)
	{
		if (!s_conn)
			return;
		const uint32_t values[4] = {(uint32_t)x, (uint32_t)y, width, height};
		xcb_configure_window(s_conn, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
		xcb_flush(s_conn);
	}

	bool Reparent(WindowId window, WindowId parent)
	{
		if (!s_conn)
			return false;
		xcb_generic_error_t* error = xcb_request_check(s_conn, xcb_reparent_window_checked(s_conn, window, parent, 0, 0));
		const bool ok = error == nullptr;
		free(error);
		return ok;
	}

	void SetEmbedMark(WindowId window)
	{
		if (!s_conn || s_embedAtom == XCB_ATOM_NONE)
			return;
		const uint32_t one = 1;
		xcb_change_property(s_conn, XCB_PROP_MODE_REPLACE, window, s_embedAtom, XCB_ATOM_CARDINAL, 32, 1, &one);
		xcb_flush(s_conn);
	}

	WindowId FindHost(WindowId window, bool waiting)
	{
		if (!s_conn)
			return 0;
		const std::optional<WindowId> parent = ParentOf(window);
		const bool parentIsRoot = !parent || *parent == 0 || IsRoot(*parent);
		if (!parentIsRoot && HasDockMark(*parent))
			return *parent;
		if (!waiting)
			return 0;
		if (const std::optional<WindowId> owner = TransientFor(window); owner && *owner != 0 && !IsOwnWindow(*owner) && WindowExists(*owner))
			return *owner;
		if (parentIsRoot || IsOwnWindow(*parent))
			return 0;
		return *parent;
	}

	void Flush()
	{
		if (s_conn)
			xcb_flush(s_conn);
	}
}
