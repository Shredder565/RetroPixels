// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/listctrl.h>

// Virtual list of all stored RPL symbols sorted by address (name, address, module).
// The filter keeps symbols whose "module + name" contains the filter text,
// case-insensitively. Activating a row jumps the disassembly there; right-click
// copies the address.
class SymbolListCtrl : public wxListView
{
public:
	SymbolListCtrl(wxWindow* parent, const wxWindowID& id, const wxPoint& pos, const wxSize& size);
	void OnGameLoaded();

	void ChangeListFilter(wxString filter);

private:
	struct Row
	{
		MPTR address;
		wxString name;
		wxString module;
		wxString haystack; // lower-case module + name, for filtering
	};

	wxString OnGetItemText(long item, long column) const override;
	void ApplyFilter();
	const Row* SelectedRow() const;
	void OnActivated(wxListEvent& event);
	void OnRightClick(wxListEvent& event);

	std::vector<Row> m_rows;         // sorted by address, one per address
	std::vector<size_t> m_shownRows; // indices into m_rows passing the filter
	wxString m_filter;               // lower case
};
