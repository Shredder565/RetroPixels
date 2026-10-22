// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/dialog.h>

// Imports a zipped save (as written by the title manager's export) into the
// mlc save folder of one account. ShowModal() returns wxOK on success and
// GetTargetPersistentId() then names the account the save went to.
class SaveImportWindow : public wxDialog
{
public:
	SaveImportWindow(wxWindow* parent, uint64 title_id);

	void EndModal(int retCode) override;

	uint32 GetTargetPersistentId() const { return m_target_id; }

private:
	void OnImport(wxCommandEvent& event);
	void Finish(int result);
	bool ConfirmTitleOfArchive(const std::string& zipPath);
	bool ExtractArchive(const std::string& zipPath, const fs::path& destination);
	void AddSaveInfoEntry(uint32 persistentId);

	class wxFilePickerCtrl* m_sourcePicker;
	class wxComboBox* m_targetCombo;

	uint32 m_target_id = 0;
	const uint64 m_title_id;
	int m_result = wxCANCEL;
};
