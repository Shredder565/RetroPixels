// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/frame.h>

class DebuggerWindow2;
class wxListEvent;
class wxListView;

// Lists all breakpoints (enabled checkbox, address, type, comment). Double-click
// the address to show it in the disassembly or the comment to edit it (the
// log message for logging breakpoints); right-click creates or deletes breakpoints.
class BreakpointWindow : public wxFrame
{
public:
	BreakpointWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size);
	virtual ~BreakpointWindow();

	void OnMainMove(const wxPoint& position, const wxSize& main_size);
	void OnUpdateView();
	void OnGameLoaded();

private:
	void OnBreakpointToggled(wxListEvent& event);
	void OnLeftDClick(wxMouseEvent& event);
	void OnRightDown(wxMouseEvent& event);
	void OnContextMenuClick(wxCommandEvent& evt);
	void OnContextMenuClickSelected(wxCommandEvent& evt);
	void EditComment(long row);

	wxListView* m_list;
};
