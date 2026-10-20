// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/panels/WiimoteInputPanel.h"

#include <wx/checkbox.h>
#include <wx/gbsizer.h>
#include <wx/slider.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "input/emulated/WiimoteController.h"
#include "wxgui/components/wxInputDraw.h"

namespace
{
	using Btn = WiimoteController::ButtonId;

	wxString Label(Btn id)
	{
		return wxGetTranslation(wxString::FromUTF8(WiimoteController::get_button_name(id)));
	}

	constexpr Btn kRemoteButtons[] = {WiimoteController::kButtonId_A, WiimoteController::kButtonId_B, WiimoteController::kButtonId_1, WiimoteController::kButtonId_2,
									  WiimoteController::kButtonId_Plus, WiimoteController::kButtonId_Minus, WiimoteController::kButtonId_Home};
	constexpr Btn kDpad[] = {WiimoteController::kButtonId_Up, WiimoteController::kButtonId_Down, WiimoteController::kButtonId_Left, WiimoteController::kButtonId_Right};
	// kButtonId_None leaves an empty row between the buttons and the stick
	constexpr Btn kNunchuck[] = {WiimoteController::kButtonId_Nunchuck_C, WiimoteController::kButtonId_Nunchuck_Z, WiimoteController::kButtonId_None,
								 WiimoteController::kButtonId_Nunchuck_Up, WiimoteController::kButtonId_Nunchuck_Down,
								 WiimoteController::kButtonId_Nunchuck_Left, WiimoteController::kButtonId_Nunchuck_Right};

	constexpr int kSeparatorFlags = wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND;

	bool HasMotionPlus(WPADDeviceType type)
	{
		return type == kWAPDevMPLS || type == kWAPDevMPLSFreeStyle || type == kWAPDevMPLSClassic;
	}
}

WiimoteInputPanel::WiimoteInputPanel(wxWindow* parent)
	: InputPanel(parent)
{
	// extension selection
	auto* extensions = new wxBoxSizer(wxHORIZONTAL);
	extensions->Add(new wxStaticText(this, wxID_ANY, _("Extensions:")));
	extensions->AddSpacer(10);
	m_motionPlus = new wxCheckBox(this, wxID_ANY, _("MotionPlus"));
	m_nunchuck = new wxCheckBox(this, wxID_ANY, _("Nunchuck"));
	m_classic = new wxCheckBox(this, wxID_ANY, _("Classic"));
	m_classic->Hide();
	for (wxCheckBox* box : {m_motionPlus, m_nunchuck, m_classic})
	{
		box->Bind(wxEVT_CHECKBOX, &WiimoteInputPanel::OnExtensionToggled, this);
		extensions->Add(box);
	}
	auto* top = new wxBoxSizer(wxHORIZONTAL);
	top->Add(extensions, wxSizerFlags(0).Align(wxALIGN_CENTER_VERTICAL));

	auto* grid = new wxGridBagSizer();

	int row = 1;
	for (Btn id : kRemoteButtons)
		AddMappingRow(grid, row++, 0, id, Label(id));
	AddSeparator(grid, 2, kSeparatorFlags);

	AddHeading(grid, 0, 3, _("D-pad"));
	row = 1;
	for (Btn id : kDpad)
		AddMappingRow(grid, row++, 3, id, Label(id));

	// remote speaker volume: shown for completeness, not emulated
	{
		constexpr int kVolumeRow = 8;
		auto* caption = new wxStaticText(this, wxID_ANY, _("Volume"));
		auto* slider = new wxSlider(this, wxID_ANY, 0, 0, 100);
		auto* percent = new wxStaticText(this, wxID_ANY, wxString::Format("%d%%", 0));
		caption->Disable();
		slider->Disable();
		percent->Disable();
		grid->Add(caption, wxGBPosition(kVolumeRow, 3), wxDefaultSpan, wxALL, 5);
		grid->Add(slider, wxGBPosition(kVolumeRow, 4), wxDefaultSpan, wxTOP | wxBOTTOM | wxEXPAND, 5);
		grid->Add(percent, wxGBPosition(kVolumeRow, 5), wxDefaultSpan, wxALL, 5);
	}
	AddSeparator(grid, 6, kSeparatorFlags);

	AddHeading(grid, 0, 7, _("Nunchuck"));
	const bool nunchuckOn = m_nunchuck->GetValue();
	row = 0;
	for (Btn id : kNunchuck)
	{
		++row;
		if (id == WiimoteController::kButtonId_None)
			continue;
		wxTextCtrl* field = AddMappingRow(grid, row, 7, id, Label(id));
		field->Enable(nunchuckOn);
		m_nunchuckControls.push_back(field);
	}
	m_nunchuckStick = new wxInputDraw(this, wxID_ANY, wxDefaultPosition, {60, 60});
	m_nunchuckStick->Enable(nunchuckOn);
	grid->Add(5, 0, wxGBPosition(3, 10), wxDefaultSpan, wxTOP | wxBOTTOM | wxEXPAND | wxALIGN_CENTER, 5);
	grid->Add(m_nunchuckStick, wxGBPosition(3, 11), wxGBSpan(4, 1), wxTOP | wxBOTTOM | wxEXPAND | wxALIGN_CENTER, 5);
	m_nunchuckControls.push_back(m_nunchuckStick);

	auto* page = new wxBoxSizer(wxVERTICAL);
	page->Add(top, 0, wxEXPAND | wxALL, 5);
	page->Add(new wxStaticLine(this), 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 5);
	page->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 5);
	SetSizer(page);
	Layout();
}

void WiimoteInputPanel::ShowDeviceType(WPADDeviceType type)
{
	m_device_type = type;

	const bool nunchuck = type == kWAPDevFreestyle || type == kWAPDevMPLSFreeStyle;
	const bool classic = type == kWAPDevClassic || type == kWAPDevMPLSClassic;
	m_motionPlus->SetValue(HasMotionPlus(type));
	m_nunchuck->SetValue(nunchuck);
	m_classic->SetValue(classic);
	for (wxWindow* control : m_nunchuckControls)
		control->Enable(nunchuck);
}

void WiimoteInputPanel::OnExtensionToggled(wxCommandEvent& event)
{
	const bool motionPlus = m_motionPlus->GetValue();
	WPADDeviceType type;
	if (m_nunchuck->GetValue())
		type = motionPlus ? kWAPDevMPLSFreeStyle : kWAPDevFreestyle;
	else if (m_classic->GetValue())
		type = motionPlus ? kWAPDevMPLSClassic : kWAPDevClassic;
	else
		type = motionPlus ? kWAPDevMPLS : kWAPDevCore;
	ShowDeviceType(type);
}

void WiimoteInputPanel::on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller)
{
	// the checkboxes are authoritative while the page is open
	if (emulated_controller)
	{
		const auto wiimote = std::dynamic_pointer_cast<WiimoteController>(emulated_controller);
		wxASSERT(wiimote);
		if (wiimote)
			wiimote->set_device_type(m_device_type);
	}

	InputPanel::on_timer(emulated_controller, controller);

	if (emulated_controller)
		m_nunchuckStick->SetAxisValue(emulated_controller->get_axis());
}

void WiimoteInputPanel::load_controller(const EmulatedControllerPtr& emulated_controller)
{
	InputPanel::load_controller(emulated_controller);
	if (!emulated_controller)
		return;
	const auto wiimote = std::dynamic_pointer_cast<WiimoteController>(emulated_controller);
	wxASSERT(wiimote);
	if (wiimote)
		ShowDeviceType(wiimote->get_device_type());
}
