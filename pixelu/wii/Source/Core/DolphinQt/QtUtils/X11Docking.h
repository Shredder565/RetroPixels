// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QPoint>
#include <qwindowdefs.h>

// PixelU: docking the render window into RetroPixels on Linux (X11, also under XWayland).
//
// RetroPixels (embed_linux.go) looks for a window of our process (by _NET_WM_PID) that carries
// the property _RETROPIXELS_EMBED (CARDINAL, 1), mapped or not, reparents it into a window of
// its own and maps it there. That window of RetroPixels carries _RETROPIXELS_DOCK (CARDINAL,
// RetroPixels' pid), which is how we can tell we are docked. Stop is a WM_DELETE_WINDOW client
// message, which Qt turns into a close event.
//
// To keep the window from appearing on the desktop while it waits, it is moved into a 1x1
// window of ours that is never mapped (the "holder") before Qt shows it: Qt maps it as usual,
// but inside an unmapped parent it is not viewable, and since its parent is not the root
// window the window manager never sees it (no frame, no task bar entry, no flash). Qt's own
// idea of the window (shown, its size) stays as it is, so nothing else needs to know.
//
// All of this uses Qt's own X connection (xcb) and does nothing when Qt does not run on X11.
// GUI thread only.
namespace QtUtils::X11Docking
{
// True when Qt runs on X11 (the xcb platform).
bool IsAvailable();

// Marks `window` as the game screen RetroPixels should dock and moves it into the holder.
// Call before the window is first shown. False when that was not possible (the window then
// shows on the desktop as usual).
bool Prepare(WId window);

// True when `window`'s parent is RetroPixels' dock window.
bool IsDocked(WId window);

// Gives up waiting: drops the mark and puts `window` on the desktop at `position` (root
// coordinates of its top-left corner), where the window manager takes it like any new window.
void Release(WId window, QPoint position);
}  // namespace QtUtils::X11Docking
