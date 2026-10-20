// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/dialogs/CreateAccount/wxCreateAccountDialog.h"
#include "Cafe/Account/Account.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/button.h>
#include <wx/textctrl.h>
#include <wx/msgdlg.h>
#include <helpers/wxHelpers.h>
#include "util/helpers/helpers.h"

wxCreateAccountDialog::wxCreateAccountDialog(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Create new account"))
{
	// two columns: label | input, buttons right-aligned in the last row
	auto* grid = new wxFlexGridSizer(0, 2, 0, 0);
	grid->AddGrowableCol(1);
	grid->SetFlexibleDirection(wxBOTH);
	grid->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

	grid->Add(new wxStaticText(this, wxID_ANY, "PersistentId"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
	m_idInput = new wxTextCtrl(this, wxID_ANY, fmt::format("{:x}", Account::GetNextPersistentId()));
	m_idInput->SetToolTip(_("The persistent id is the internal folder name used for your saves. Only change this if you are importing saves from a Wii U with a specific id"));
	grid->Add(m_idInput, 1, wxALL | wxEXPAND, 5);

	grid->Add(new wxStaticText(this, wxID_ANY, _("Mii name")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
	m_nameInput = new wxTextCtrl(this, wxID_ANY);
	m_nameInput->SetFocus();
	m_nameInput->SetMaxLength(10);
	grid->Add(m_nameInput, 1, wxALL | wxEXPAND, 5);

	grid->Add(0, 0, 1, wxEXPAND, 5);
	auto* buttons = new wxBoxSizer(wxHORIZONTAL);
	auto* ok = new wxButton(this, wxID_ANY, _("OK"));
	ok->Bind(wxEVT_BUTTON, &wxCreateAccountDialog::OnOK, this);
	buttons->Add(ok, 0, wxALL, 5);
	auto* cancel = new wxButton(this, wxID_ANY, _("Cancel"));
	cancel->Bind(wxEVT_BUTTON, &wxCreateAccountDialog::OnCancel, this);
	buttons->Add(cancel, 0, wxALL, 5);
	grid->Add(buttons, 1, wxALIGN_RIGHT, 5);

	SetSizerAndFit(grid);
	wxWindowBase::Layout();
}

uint32 wxCreateAccountDialog::GetPersistentId() const
{
	return ConvertString<uint32>(m_idInput->GetValue().ToStdString(), 16);
}

wxString wxCreateAccountDialog::GetMiiName() const
{
	return m_nameInput->GetValue();
}

void wxCreateAccountDialog::ShowError(const wxString& text)
{
	wxMessageBox(text, _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
}

void wxCreateAccountDialog::OnOK(wxCommandEvent& /*event*/)
{
	if (m_idInput->IsEmpty())
	{
		ShowError(_("No persistent id entered!"));
		return;
	}
	const uint32 id = GetPersistentId();
	if (id < Account::kMinPersistendId)
	{
		ShowError(formatWxString(_("The persistent id must be greater than {:x}!"), Account::kMinPersistendId));
		return;
	}
	// GetAccount falls back to another account when the id is unknown, so compare the ids
	const Account& existing = Account::GetAccount(id);
	if (existing.GetPersistentId() == id)
	{
		const std::wstring text = fmt::format(fmt::runtime(_("The persistent id {:x} is already in use by account {}!").ToStdWstring()),
			existing.GetPersistentId(), std::wstring{existing.GetMiiName()});
		ShowError(text);
		return;
	}
	if (m_nameInput->IsEmpty())
	{
		ShowError(_("Account name may not be empty!"));
		return;
	}
	EndModal(wxID_OK);
}

void wxCreateAccountDialog::OnCancel(wxCommandEvent& /*event*/)
{
	EndModal(wxID_CANCEL);
}
