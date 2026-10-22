// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "wxgui/debugger/DumpCtrl.h"

class DebuggerWindow2;

// Floating debugger window holding the memory dump view; docks below the main window when pinned.
class DumpWindow : public wxFrame
{
public:
	DumpWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size);

	void OnMainMove(const wxPoint& position, const wxSize& main_size);
	void OnGameLoaded();

private:
	DumpCtrl* m_view;
};
