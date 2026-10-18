// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// PixelU: --rp-embed / --rp-parent, for RetroPixels showing the game inside its own window.
// With --rp-embed the main window is a plain resizable window with only the game picture in it
// (no menu bar, game list or optional dialogs, no fullscreen switching), titled "PixelU - <game>".
// With --rp-parent <HWND> (Windows) it is also made a child of that window, filling its client area.
// Without --rp-parent, RetroPixels docks the window itself (SetParent). Until then it is never seen
// on its own: it is a tool window (no taskbar button, not in Alt+Tab), shown off-screen at -32000,-32000
// without being activated. No other PixelU window opens by itself: no GamePad window, no wxWidgets
// log popups; errors that must be shown use ShowMessage.

class wxFrame;
class wxString;

namespace RPEmbed
{
	bool Enabled();

	// style of the main frame
	long FrameStyle(long defaultStyle);

	// --rp-parent: make the main frame a child of the given window and keep it sized to it.
	// Does nothing without --rp-parent, on other platforms, or if the window doesn't exist.
	void AttachToParent(wxFrame* frame);

	// Right before the main frame is shown the first time. Not yet docked (no --rp-parent), it is
	// moved off-screen and then watched: once RetroPixels makes it a child window it behaves as usual.
	// If nobody docks it within 20 seconds (RetroPixels couldn't), it comes on screen as a normal window.
	void BeforeShow(wxFrame* frame);

	// the main frame is a child window (of RetroPixels' window)
	bool IsDocked();

	// Without --rp-embed: wxMessageBox(message, caption, style), as before.
	// With it: the message is logged, and shown in a message box owned by the game window once that is
	// docked, so it stays on top of RetroPixels and gets no taskbar button. Before the game window
	// exists (errors at startup) it is only logged. Any thread.
	void ShowMessage(const wxString& message, const wxString& caption, long style);

	// --rp-embed: wxWidgets log messages (wxLogError etc.) go to log.txt instead of a popup
	void RouteWxLog();
}
