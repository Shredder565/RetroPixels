// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// PixelU: --rp-embed / --rp-parent, for RetroPixels showing the game inside its own window.
// With --rp-embed the main window is a plain resizable window with only the game picture in it
// (no menu bar, game list or optional dialogs, no fullscreen switching), titled "PixelU - <game>".
// With --rp-parent <HWND> (Windows) it is also made a child of that window, filling its client area.

class wxFrame;

namespace RPEmbed
{
	bool Enabled();

	// style of the main frame
	long FrameStyle(long defaultStyle);

	// --rp-parent: make the main frame a child of the given window and keep it sized to it.
	// Does nothing without --rp-parent, on other platforms, or if the window doesn't exist.
	void AttachToParent(wxFrame* frame);
}
