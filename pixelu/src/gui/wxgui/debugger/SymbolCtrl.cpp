// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/debugger/SymbolCtrl.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include <wx/listctrl.h>
#include <wx/clipbrd.h>

namespace
{
	enum : long
	{
		COL_NAME,
		COL_ADDRESS,
		COL_MODULE,
	};
}

SymbolListCtrl::SymbolListCtrl(wxWindow* parent, const wxWindowID& id, const wxPoint& pos, const wxSize& size)
	: wxListView(parent, id, pos, size, wxLC_REPORT | wxLC_VIRTUAL)
{
	InsertColumn(COL_NAME, _("Name"), wxLIST_FORMAT_LEFT, 200);
	InsertColumn(COL_ADDRESS, _("Address"), wxLIST_FORMAT_RIGHT, 75);
	InsertColumn(COL_MODULE, _("Module"), wxLIST_FORMAT_RIGHT, 75);

	Bind(wxEVT_LIST_ITEM_ACTIVATED, &SymbolListCtrl::OnActivated, this);
	Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &SymbolListCtrl::OnRightClick, this);

	OnGameLoaded();
}

void SymbolListCtrl::OnGameLoaded()
{
	m_rows.clear();
	m_shownRows.clear();
	// one symbol per address; the list shows them in address order
	const auto& symbols = rplSymbolStorage_lockSymbolMap();
	m_rows.reserve(symbols.size());
	for (const auto& [address, sym] : symbols)
	{
		if (sym == nullptr || sym->symbolName == nullptr)
			continue;
		Row r;
		r.address = address;
		r.name = wxString::FromAscii((const char*)sym->symbolName);
		r.module = wxString::FromAscii((const char*)sym->libName);
		r.haystack = (r.module + r.name).Lower();
		m_rows.push_back(std::move(r));
	}
	rplSymbolStorage_unlockSymbolMap();
	std::sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) { return a.address < b.address; });
	ApplyFilter();
}

void SymbolListCtrl::ApplyFilter()
{
	m_shownRows.clear();
	m_shownRows.reserve(m_rows.size());
	for (size_t i = 0; i < m_rows.size(); i++)
	{
		if (m_filter.IsEmpty() || m_rows[i].haystack.Contains(m_filter))
			m_shownRows.push_back(i);
	}

	const long shown = (long)m_shownRows.size();
	SetItemCount(shown);
	if (shown == 0)
	{
		Refresh();
		return;
	}
	const long top = std::min<long>(GetTopItem(), shown - 1);
	RefreshItems(top, std::min<long>(shown - 1, top + GetCountPerPage() + 1));
}

wxString SymbolListCtrl::OnGetItemText(long item, long column) const
{
	if (item < 0 || item >= (long)m_shownRows.size())
		return wxEmptyString;
	const Row& r = m_rows[m_shownRows[item]];
	switch (column)
	{
	case COL_NAME: return r.name;
	case COL_ADDRESS: return wxString::Format("%08x", r.address);
	case COL_MODULE: return r.module;
	default: return wxEmptyString;
	}
}

const SymbolListCtrl::Row* SymbolListCtrl::SelectedRow() const
{
	const long sel = GetFirstSelected();
	if (sel == wxNOT_FOUND || sel >= (long)m_shownRows.size())
		return nullptr;
	return &m_rows[m_shownRows[sel]];
}

void SymbolListCtrl::OnActivated(wxListEvent& /*event*/)
{
	const Row* r = SelectedRow();
	if (r != nullptr && r->address != 0)
		debugger_jumpToAddressInDisasm(r->address);
}

void SymbolListCtrl::OnRightClick(wxListEvent& /*event*/)
{
	const Row* r = SelectedRow();
	if (r == nullptr)
		return;
	wxClipboard* cb = wxClipboard::Get();
	if (!cb->Open())
		return;
	cb->SetData(new wxTextDataObject(wxString::Format("0x%08x", r->address)));
	cb->Close();
}

void SymbolListCtrl::ChangeListFilter(wxString filter)
{
	m_filter = filter.MakeLower();
	ApplyFilter();
}
