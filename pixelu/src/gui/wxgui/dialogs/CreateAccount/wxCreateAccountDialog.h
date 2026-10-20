// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <wx/dialog.h>

// Modal dialog asking for a persistent id (hex) and a Mii name for a new account.
// Returns wxID_OK only once the input is valid and the id is not taken.
class wxCreateAccountDialog : public wxDialog
{
public:
	wxCreateAccountDialog(wxWindow* parent);

	[[nodiscard]] uint32 GetPersistentId() const;
	[[nodiscard]] wxString GetMiiName() const;

private:
	void OnOK(wxCommandEvent& event);
	void OnCancel(wxCommandEvent& event);
	void ShowError(const wxString& text);

	class wxTextCtrl* m_idInput;
	class wxTextCtrl* m_nameInput;
};
