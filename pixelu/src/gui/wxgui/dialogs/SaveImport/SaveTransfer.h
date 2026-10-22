// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/dialog.h>

class wxComboBox;

// Moves one account's save of a title to another account (renames the save
// folder and updates meta/saveinfo.xml). ShowModal() returns wxOK on success.
class SaveTransfer : public wxDialog
{
public:
	SaveTransfer(wxWindow* parent, uint64 title_id, const wxString& source_account, uint32 source_id);

	void EndModal(int retCode) override;

	uint32 GetTargetPersistentId() const { return m_target_id; }

private:
	void OnTransfer(wxCommandEvent& event);
	void Finish(int result);
	fs::path UserSavePath(uint32 persistentId) const;
	bool RewriteSaveInfo(uint32 targetId);

	wxComboBox* m_targetCombo;

	uint32 m_target_id = 0;
	const uint64 m_title_id;
	const uint32 m_source_id;
	int m_result = wxCANCEL;
};
