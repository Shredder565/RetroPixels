// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/dialog.h>
#include <wx/notebook.h>
#include <wx/timer.h>

#include "input/api/InputAPI.h"

#include <boost/signals2/connection.hpp>

struct ControllerPage;
class ControllerBase;
class InputPanel;
class wxComboBox;

// The input settings dialog: one notebook tab per emulated controller slot,
// each with profile handling, the emulated controller type, the physical
// controllers feeding it and the mapping panel of that type.
class InputSettings2 : public wxDialog
{
public:
	InputSettings2(wxWindow* parent);
	~InputSettings2();

private:
	const wxString kDefaultProfileName = _("<profile name>");

	wxWindow* initialize_page(size_t index);
	ControllerPage& get_current_page_data() const;
	// physical controller selected on the current tab
	std::shared_ptr<ControllerBase> get_active_controller() const;
	InputPanel* CurrentPanel() const;
	void update_state();

	void FillProfileList(wxComboBox* profiles);
	void OnProfileTextChanged(wxCommandEvent& event);
	void LoadProfile();
	void SaveProfile();
	void DeleteProfile();

	void OnEmulatedTypeChosen(wxCommandEvent& event);
	void FillEmulatedTypeList(wxComboBox* types);

	void OnControllerChosen(wxCommandEvent& event);
	void AddController();
	void RemoveController();
	void TestConnection();
	void OpenControllerSettings();
	void OnControllersChanged();

	wxNotebook* m_notebook;
	wxTimer* m_timer;
	wxBitmap m_connected, m_disconnected, m_low_battery;
	boost::signals2::connection m_controller_changed;
};
