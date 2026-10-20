// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/panels/ClassicControllerInputPanel.h"

#include <wx/gbsizer.h>

#include "input/emulated/ClassicController.h"
#include "wxgui/components/wxInputDraw.h"

namespace
{
	using Btn = ClassicController::ButtonId;

	wxString Label(Btn id)
	{
		return wxGetTranslation(wxString::FromUTF8(ClassicController::get_button_name(id)));
	}

	constexpr Btn kFaceAndShoulder[] = {ClassicController::kButtonId_A, ClassicController::kButtonId_B, ClassicController::kButtonId_X, ClassicController::kButtonId_Y,
										ClassicController::kButtonId_L, ClassicController::kButtonId_R, ClassicController::kButtonId_ZL, ClassicController::kButtonId_ZR,
										ClassicController::kButtonId_Plus, ClassicController::kButtonId_Minus};
	// the classic controller sticks have no click
	constexpr Btn kLeftStick[] = {ClassicController::kButtonId_StickL_Up, ClassicController::kButtonId_StickL_Down,
								  ClassicController::kButtonId_StickL_Left, ClassicController::kButtonId_StickL_Right};
	constexpr Btn kRightStick[] = {ClassicController::kButtonId_StickR_Up, ClassicController::kButtonId_StickR_Down,
								   ClassicController::kButtonId_StickR_Left, ClassicController::kButtonId_StickR_Right};
	constexpr Btn kDpad[] = {ClassicController::kButtonId_Up, ClassicController::kButtonId_Down, ClassicController::kButtonId_Left, ClassicController::kButtonId_Right};
}

ClassicControllerInputPanel::ClassicControllerInputPanel(wxWindow* parent)
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
	AddSeparator(grid, 10, wxALL | wxEXPAND);

	AddHeading(grid, 0, 11, _("D-pad"));
	addColumn(11, kDpad);

	SetSizer(grid);
	Layout();
}

void ClassicControllerInputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller)
{
	InputPanel::on_timer(emulated_controller, controller);
	if (!emulated_controller)
		return;
	m_leftStick->SetAxisValue(emulated_controller->get_axis());
	m_rightStick->SetAxisValue(emulated_controller->get_rotation());
}
