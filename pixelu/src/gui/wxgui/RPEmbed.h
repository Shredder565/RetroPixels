// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// PixelU inside RetroPixels' window: --rp-embed and --rp-parent.
//
// --rp-embed: the main window holds only the game picture. No menu bar, game list, optional dialogs or
// fullscreen switching; the title is "PixelU - <game>". RetroPixels docks the window itself (SetParent).
// Until it does, the window is never seen on its own: it is a tool window (no taskbar button, not in
// Alt+Tab) placed off-screen at -32000,-32000 and shown without being activated. No other PixelU window
// opens by itself (no GamePad window, no wxWidgets log popups); errors that must be seen go through
// ShowMessage.
// --rp-parent <HWND> (Windows): the main window is also made a child of that window and kept at the
// size of its client area.

class wxFrame;
class wxString;

namespace RPEmbed
{
	// --rp-embed was given
	bool Enabled();

	// window style for the main frame (defaultStyle without --rp-embed)
	long FrameStyle(long defaultStyle);

	// --rp-parent: makes the main frame a borderless child of the given window and keeps it sized to it;
	// closes PixelU when that window goes away. Does nothing without --rp-parent, outside Windows, or
	// when the window does not exist.
	void AttachToParent(wxFrame* frame);

	// Call right before the main frame is shown for the first time. Unless it is already docked
	// (--rp-parent) it is moved off-screen and watched until RetroPixels makes it a child window. When
	// nobody docks it within 20 seconds it comes on screen as a normal window.
	void BeforeShow(wxFrame* frame);

	// the main frame is docked into RetroPixels' window
	bool IsDocked();

	// Without --rp-embed this is wxMessageBox(message, caption, style).
	// With it, the message is logged and shown in a message box owned by the game window once that is
	// docked, so it stays above RetroPixels and has no taskbar button. Before the game window exists
	// (errors during start-up) it is only logged. May be called from any thread.
	void ShowMessage(const wxString& message, const wxString& caption, long style);

	// --rp-embed: wxWidgets log output (wxLogError and the like) goes to log.txt instead of a popup
	void RouteWxLog();
}
