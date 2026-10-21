// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// The X11 side of --rp-embed and --rp-parent on Linux (see RPEmbed.h), on the XCB connection under
// GDK's Xlib display. No wxWidgets or GTK in here, so it can be tried on its own against an X server.
//
// What RetroPixels and PixelU agree on (RetroPixels' side is embed_linux.go and
// internal/pixelu/screen_linux.go):
//
// - The game window is PixelU's main window. It carries _NET_WM_PID and the property "PixelU.Screen"
//   (CARDINAL[2]: the number of the last screen request it answered, then 1 while the GamePad screen is
//   shown, else 0). Under --rp-embed it is created but left unmapped until it is docked.
// - While it waits it also carries _RETROPIXELS_EMBED (CARDINAL 1): RetroPixels docks that window even
//   though it is unmapped (embed_linux.go).
// - RetroPixels docks it by reparenting it into a window of its own (which carries _RETROPIXELS_DOCK,
//   RetroPixels' pid) and mapping it; setting WM_TRANSIENT_FOR to one of its windows, or only mapping
//   it, counts too. PixelU sees this within a tenth of a second, shows the window itself without a
//   frame, and keeps it filling the window it was reparented into. A window shown on its own after the
//   wait is still docked later when it is put into a window carrying _RETROPIXELS_DOCK.
// - The screen switch is a ClientMessage of type "PixelU.Screen" sent to the game window, format 32,
//   data[0] = 0 TV, 1 GamePad, 2 the other one, anything else only asks; data[1] = a request number.
//   PixelU answers by setting the property to {request number, GamePad shown}.
// - When the window it is docked into goes away, PixelU closes.

#include <cstdint>
#include <optional>

struct xcb_connection_t;

namespace RPEmbedX11
{
	using WindowId = uint32_t;
	using AtomId = uint32_t;

	// the name of both the screen message and the property
	constexpr const char* kScreenName = "PixelU.Screen";

	// Interns the atoms used below. False when the connection is broken.
	bool Init(xcb_connection_t* connection);

	// the atom of "PixelU.Screen" (0 before Init)
	AtomId ScreenAtom();

	// sets the "PixelU.Screen" property of the window to {requestNumber, gamePadShown}
	void PublishScreen(WindowId window, uint32_t requestNumber, bool gamePadShown);

	struct ScreenRequest
	{
		int mode; // 0 TV, 1 GamePad, 2 switch, other: only ask
		uint32_t requestNumber;
	};
	// a "PixelU.Screen" client message, from its type and its five 32-bit data words
	std::optional<ScreenRequest> ParseScreenMessage(AtomId type, int format, const long data[5]);

	// whether the window was created by this connection (X gives every client its own id range)
	bool IsOwnWindow(WindowId window);

	// whether the window still exists
	bool WindowExists(WindowId window);

	// whether the window is mapped (shown, or would be with its parents)
	bool IsMapped(WindowId window);

	// parent window, nullopt when the window is gone; the root's parent is 0
	std::optional<WindowId> ParentOf(WindowId window);

	struct Geometry
	{
		int x, y; // in the parent's coordinates
		uint32_t width, height;
		bool operator==(const Geometry&) const = default;
	};
	// place and inside size of a window; nullopt when it is gone
	std::optional<Geometry> GetGeometry(WindowId window);

	// moves/sizes a window in its parent's coordinates
	void MoveResize(WindowId window, int x, int y, uint32_t width, uint32_t height);

	// makes window a child of parent at 0,0; false when either is gone
	bool Reparent(WindowId window, WindowId parent);

	// Marks the window as the one RetroPixels docks even while it is unmapped (_RETROPIXELS_EMBED = 1).
	void SetEmbedMark(WindowId window);

	// What the game window is docked into, 0 for nothing. Always: a parent carrying _RETROPIXELS_DOCK
	// (RetroPixels' box). While waiting unmapped (waiting = true) also: the WM_TRANSIENT_FOR window or the
	// parent when either belongs to another program (once a window manager frames a mapped window its
	// parent is the frame, which tells nothing).
	WindowId FindHost(WindowId window, bool waiting);

	// sends everything queued to the server
	void Flush();
}
