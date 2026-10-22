// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/CemuUpdateWindow.h"

#include <wx/sizer.h>
#include <wx/button.h>

CemuUpdateWindow::CemuUpdateWindow(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("PixelU update"), wxDefaultPosition, wxDefaultSize,
			   wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX)
{
	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(new wxStaticText(this, wxID_ANY, _("PixelU is updated together with RetroPixels.\nThere is no separate update to download here.")), 0, wxALL, 10);

	auto* close = new wxButton(this, wxID_CLOSE, _("Close"));
	close->Bind(wxEVT_BUTTON, &CemuUpdateWindow::OnCloseButton, this);
	layout->Add(close, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);

	SetSizerAndFit(layout);
	Centre(wxBOTH);
	SetEscapeId(wxID_CLOSE);
}

CemuUpdateWindow::~CemuUpdateWindow() = default;

std::future<bool> CemuUpdateWindow::IsUpdateAvailableAsync()
{
	std::promise<bool> none;
	none.set_value(false);
	return none.get_future();
}

void CemuUpdateWindow::OnCloseButton(wxCommandEvent&)
{
	EndModal(wxID_CLOSE);
}
