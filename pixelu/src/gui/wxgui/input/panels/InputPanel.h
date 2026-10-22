// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/panel.h>
#include <wx/settings.h>

#include "input/emulated/EmulatedController.h"
#include "input/api/Controller.h"

#include "wxgui/wxHelper.h"

class ControllerBase;
class wxGridBagSizer;
class wxInputDraw;
class wxTextCtrl;
class wxComboBox;

// Base for the per-controller-type mapping pages of the input settings.
// Each mapping is a read-only text field whose client data holds the emulated
// button id. Focusing a field arms it; the next input from the selected
// physical controller is bound to it and focus moves on to the next field.
// Right click or Escape on an armed field clears its mapping.
class InputPanel : public wxPanel
{
public:
	const wxColour kKeyColourNormalMode = GetBackgroundColour();
	const wxColour kKeyColourEditMode = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
	const wxColour kKeyColourActiveMode = wxHelper::CalculateAccentColour(GetBackgroundColour());

	InputPanel(wxWindow* parent);

	// polled by the settings window while it is open
	virtual void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller);
	void on_left_click(wxMouseEvent& event);

	// empties every field
	void reset_configuration();
	virtual void load_controller(const EmulatedControllerPtr& controller);

	// highlights fields that are mapped to a controller other than the given one
	void set_selected_controller(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller_base);
	void reset_colours();

protected:
	wxTextCtrl* AddMappingRow(wxGridBagSizer* sizer, int row, int column, uint64 mapping, const wxString& label);
	void AddHeading(wxGridBagSizer* sizer, int row, int column, const wxString& text);
	void AddSeparator(wxGridBagSizer* sizer, int column, int flags);
	// 60x60 stick preview below a column of mappings
	wxInputDraw* AddStickPreview(wxGridBagSizer* sizer, int row, int column);
	void MakeMappingField(wxTextCtrl* field, uint64 mapping);

private:
	template<typename TFunc>
	void ForEachField(TFunc&& func);
	wxTextCtrl* ArmedField();
	void Arm(wxFocusEvent& event);
	void Disarm(wxFocusEvent& event);
	void OnFieldRightDown(wxMouseEvent& event);
	bool DisarmCurrent();
	void PaintField(wxTextCtrl* field, const wxColour& colour);

	int m_armedId = wxID_NONE;
	bool m_clearRequested = false;
	std::unordered_map<int, wxColour> m_savedColour; // colour a field returns to when disarmed
};
