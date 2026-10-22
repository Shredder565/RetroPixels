// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/debugger/SymbolWindow.h"
#include "wxgui/debugger/DebuggerWindow2.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"

SymbolWindow::SymbolWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size)
	: wxFrame(&parent, wxID_ANY, _("Symbols"), wxDefaultPosition, wxSize(600, 250), wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT)
{
	auto* layout = new wxBoxSizer(wxVERTICAL);
	m_list = new SymbolListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
	layout->Add(m_list, 1, wxEXPAND);
	m_filterBox = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_NO_VSCROLL);
	m_filterBox->Bind(wxEVT_TEXT, &SymbolWindow::OnFilterChanged, this);
	layout->Add(m_filterBox, 0, wxALL | wxEXPAND, 5);
	SetSizer(layout);
	wxWindowBase::Layout();
	Centre(wxHORIZONTAL);

	if (parent.GetConfig().data().pin_to_main)
		OnMainMove(main_position, main_size);
}

void SymbolWindow::OnMainMove(const wxPoint& main_position, const wxSize& /*main_size*/)
{
	// left of the main window, level with its top edge
	SetSize(wxSize(420, 250));
	SetPosition(wxPoint(main_position.x - 420, main_position.y));
}

void SymbolWindow::OnGameLoaded()
{
	m_list->OnGameLoaded();
}

void SymbolWindow::OnFilterChanged(wxCommandEvent& /*event*/)
{
	m_list->ChangeListFilter(m_filterBox->GetValue());
}
