// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/panels/VPADInputPanel.h"

#include <wx/checkbox.h>
#include <wx/gbsizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>

#include "input/emulated/VPADController.h"
#include "wxgui/components/wxInputDraw.h"

namespace
{
	using Btn = VPADController::ButtonId;

	wxString Label(Btn id)
	{
		return wxGetTranslation(wxString::FromUTF8(VPADController::get_button_name(id)));
	}

	constexpr Btn kFaceAndShoulder[] = {VPADController::kButtonId_A, VPADController::kButtonId_B, VPADController::kButtonId_X, VPADController::kButtonId_Y,
										VPADController::kButtonId_L, VPADController::kButtonId_R, VPADController::kButtonId_ZL, VPADController::kButtonId_ZR,
										VPADController::kButtonId_Plus, VPADController::kButtonId_Minus};
	constexpr Btn kLeftStick[] = {VPADController::kButtonId_StickL, VPADController::kButtonId_StickL_Up, VPADController::kButtonId_StickL_Down,
								  VPADController::kButtonId_StickL_Left, VPADController::kButtonId_StickL_Right};
	constexpr Btn kRightStick[] = {VPADController::kButtonId_StickR, VPADController::kButtonId_StickR_Up, VPADController::kButtonId_StickR_Down,
								   VPADController::kButtonId_StickR_Left, VPADController::kButtonId_StickR_Right};
	constexpr Btn kDpad[] = {VPADController::kButtonId_Up, VPADController::kButtonId_Down, VPADController::kButtonId_Left, VPADController::kButtonId_Right};

	constexpr int kLastRow = 10;
}

VPADInputPanel::VPADInputPanel(wxWindow* parent)
	: InputPanel(parent)
{
	auto* grid = new wxGridBagSizer();
	auto addColumn = [&](int column, const auto& ids) {
		int row = 1;
		for (Btn id : ids)
			AddMappingRow(grid, row++, column, id, Label(id));
		return row;
	};

	addColumn(0, kFaceAndShoulder);
	AddSeparator(grid, 2, wxALL | wxEXPAND);

	AddHeading(grid, 0, 3, _("Left Axis"));
	m_leftStick = AddStickPreview(grid, addColumn(3, kLeftStick), 4);
	AddSeparator(grid, 6, wxALL | wxEXPAND);

	AddHeading(grid, 0, 7, _("Right Axis"));
	m_rightStick = AddStickPreview(grid, addColumn(7, kRightStick), 8);

	// gamepad speaker volume: shown for completeness, not emulated
	{
		auto* caption = new wxStaticText(this, wxID_ANY, _("Volume"));
		auto* slider = new wxSlider(this, wxID_ANY, 0, 0, 100);
		auto* percent = new wxStaticText(this, wxID_ANY, wxString::Format("%d%%", 0));
		caption->Disable();
		slider->Disable();
		percent->Disable();
		grid->Add(caption, wxGBPosition(kLastRow, 7), wxDefaultSpan, wxALL, 5);
		grid->Add(slider, wxGBPosition(kLastRow, 8), wxDefaultSpan, wxTOP | wxBOTTOM | wxEXPAND, 5);
		grid->Add(percent, wxGBPosition(kLastRow, 9), wxDefaultSpan, wxALL, 5);
	}
	AddSeparator(grid, 10, wxALL | wxEXPAND);

	AddHeading(grid, 0, 11, _("D-pad"));
	addColumn(11, kDpad);

	AddMappingRow(grid, kLastRow - 2, 11, VPADController::kButtonId_Mic, _("blow mic"));
	AddMappingRow(grid, kLastRow - 1, 11, VPADController::kButtonId_Screen, _("show screen"));

	const wxString toggleTip = _("Makes the \"show screen\" button toggle between the TV and gamepad screens");
	auto* toggleCaption = new wxStaticText(this, wxID_ANY, _("toggle screen"));
	toggleCaption->SetToolTip(toggleTip);
	grid->Add(toggleCaption, wxGBPosition(kLastRow, 11), wxDefaultSpan, wxALL | wxALIGN_CENTER_VERTICAL, 5);
	m_screenToggle = new wxCheckBox(this, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	m_screenToggle->SetToolTip(toggleTip);
	grid->Add(m_screenToggle, wxGBPosition(kLastRow, 12), wxDefaultSpan, wxALL | wxEXPAND, 5);

	SetSizer(grid);
	Layout();
}

void VPADInputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller)
{
	InputPanel::on_timer(emulated_controller, controller);
	if (!emulated_controller)
		return;
	static_cast<VPADController*>(emulated_controller.get())->set_screen_toggle(m_screenToggle->GetValue());
	m_leftStick->SetAxisValue(emulated_controller->get_axis());
	m_rightStick->SetAxisValue(emulated_controller->get_rotation());
}

void VPADInputPanel::load_controller(const EmulatedControllerPtr& controller)
{
	InputPanel::load_controller(controller);
	if (controller)
		m_screenToggle->SetValue(static_cast<VPADController*>(controller.get())->is_screen_active_toggle());
}
