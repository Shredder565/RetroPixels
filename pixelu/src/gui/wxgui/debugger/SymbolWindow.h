// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "wxgui/debugger/SymbolCtrl.h"
#include <wx/frame.h>

class DebuggerWindow2;

// Floating debugger window: the symbol list with a filter box below it.
class SymbolWindow : public wxFrame
{
public:
	SymbolWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size);

	void OnMainMove(const wxPoint& position, const wxSize& main_size);
	void OnGameLoaded();

	void OnLeftDClick(wxListEvent& event);

private:
	void OnFilterChanged(wxCommandEvent& event);

	wxTextCtrl* m_filterBox;
	SymbolListCtrl* m_list;
};
