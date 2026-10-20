// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/debugger/DumpWindow.h"

#include "wxgui/debugger/DebuggerWindow2.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "wxgui/debugger/DumpCtrl.h"

namespace
{
	const wxSize kDumpWindowSize(600, 250);
}

DumpWindow::DumpWindow(DebuggerWindow2& parent, const wxPoint& main_position, const wxSize& main_size)
	: wxFrame(&parent, wxID_ANY, _("Memory Dump"), wxDefaultPosition, kDumpWindowSize, wxSYSTEM_MENU | wxCAPTION | wxCLIP_CHILDREN | wxRESIZE_BORDER | wxFRAME_FLOAT_ON_PARENT)
{
	auto* layout = new wxBoxSizer(wxVERTICAL);
	m_view = new DumpCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxScrolledWindowStyle);
	layout->Add(m_view, 1, wxEXPAND);
	SetSizer(layout);
	wxWindowBase::Layout();
	Centre(wxBOTH);

	if (parent.GetConfig().data().pin_to_main)
		OnMainMove(main_position, main_size);
}

void DumpWindow::OnMainMove(const wxPoint& main_position, const wxSize& main_size)
{
	// directly below the main window, aligned to its left edge
	SetSize(kDumpWindowSize);
	SetPosition(wxPoint(main_position.x, main_position.y + main_size.GetHeight()));
}

void DumpWindow::OnGameLoaded()
{
	m_view->Init();
}
