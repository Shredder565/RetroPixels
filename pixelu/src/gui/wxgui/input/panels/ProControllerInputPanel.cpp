// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/panels/ProControllerInputPanel.h"

#include <wx/gbsizer.h>

#include "input/emulated/ProController.h"
#include "wxgui/components/wxInputDraw.h"

namespace
{
	using Btn = ProController::ButtonId;

	wxString Label(Btn id)
	{
		return wxGetTranslation(wxString::FromUTF8(ProController::get_button_name(id)));
	}

	// mappings below each heading; the first group has no heading
	constexpr Btn kFaceAndShoulder[] = {ProController::kButtonId_A, ProController::kButtonId_B, ProController::kButtonId_X, ProController::kButtonId_Y,
										ProController::kButtonId_L, ProController::kButtonId_R, ProController::kButtonId_ZL, ProController::kButtonId_ZR,
										ProController::kButtonId_Plus, ProController::kButtonId_Minus};
	constexpr Btn kLeftStick[] = {ProController::kButtonId_StickL, ProController::kButtonId_StickL_Up, ProController::kButtonId_StickL_Down,
								  ProController::kButtonId_StickL_Left, ProController::kButtonId_StickL_Right};
	constexpr Btn kRightStick[] = {ProController::kButtonId_StickR, ProController::kButtonId_StickR_Up, ProController::kButtonId_StickR_Down,
								   ProController::kButtonId_StickR_Left, ProController::kButtonId_StickR_Right};
	constexpr Btn kDpad[] = {ProController::kButtonId_Up, ProController::kButtonId_Down, ProController::kButtonId_Left, ProController::kButtonId_Right};
}

ProControllerInputPanel::ProControllerInputPanel(wxWindow* parent)
	: InputPanel(parent)
{
	auto* grid = new wxGridBagSizer();
	// returns the first free row under the list
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
	AddSeparator(grid, 10, wxALL | wxEXPAND);

	AddHeading(grid, 0, 11, _("D-pad"));
	addColumn(11, kDpad);

	SetSizerAndFit(grid);
}

void ProControllerInputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller)
{
	InputPanel::on_timer(emulated_controller, controller);
	if (!emulated_controller)
		return;
	m_leftStick->SetAxisValue(emulated_controller->get_axis());
	m_rightStick->SetAxisValue(emulated_controller->get_rotation());
}
