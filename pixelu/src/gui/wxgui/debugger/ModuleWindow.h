// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/frame.h>

class DebuggerWindow2;
class wxListView;

// Lists the loaded RPL modules and the code caves of applied graphic pack
// patch groups (name, text address, size). Double-click jumps to the address.
class ModuleWindow : public wxFrame
{
public:
	ModuleWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size);

	void OnMainMove(const wxPoint& position, const wxSize& main_size);
	void OnGameLoaded();

private:
	void OnLeftDClick(wxMouseEvent& event);
	void AddRow(const wxString& name, uint32 address, uint32 size);

	wxListView* m_list;
};
