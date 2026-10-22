// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/settings/WiimoteControllerSettings.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include "wxgui/components/wxInputDraw.h"

#ifdef SUPPORTS_WIIMOTE

namespace
{
	// "caption  [slider]  value" across three sizer cells; the value label uses
	// format and onChange (optional) gets every new position
	wxSlider* AddSliderRow(wxWindow* parent, wxSizer* sizer, const wxString& caption, int value, int min, int max,
						   const char* format, std::function<void(int)> onChange)
	{
		sizer->Add(new wxStaticText(parent, wxID_ANY, caption), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		auto* slider = new wxSlider(parent, wxID_ANY, value, min, max);
		sizer->Add(slider, 1, wxALL | wxEXPAND, 5);
		auto* valueText = new wxStaticText(parent, wxID_ANY, wxString::Format(format, value));
		sizer->Add(valueText, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		slider->Bind(wxEVT_SLIDER, [slider, valueText, format, onChange = std::move(onChange)](wxCommandEvent&) {
			valueText->SetLabel(wxString::Format(format, slider->GetValue()));
			if (onChange)
				onChange(slider->GetValue());
		});
		return slider;
	}

	void AddDeadzoneRow(wxWindow* parent, wxSizer* sizer, ControllerBase::AxisSetting& setting)
	{
		AddSliderRow(parent, sizer, _("Deadzone"), (int)(setting.deadzone * 100), 0, 100, "%d%%",
					 [&setting](int v) { setting.deadzone = (float)v / 100.0f; });
	}

	void AddRangeRow(wxWindow* parent, wxSizer* sizer, ControllerBase::AxisSetting& setting)
	{
		AddSliderRow(parent, sizer, _("Range"), (int)(setting.range * 100), 50, 200, "%d%%",
					 [&setting](int v) { setting.range = (float)v / 100.0f; });
	}

	wxInputDraw* AddPreview(wxWindow* parent, wxSizer* sizer)
	{
		auto* preview = new wxInputDraw(parent, wxID_ANY, wxDefaultPosition, {60, 60});
		sizer->Add(preview, 0, wxTOP | wxBOTTOM | wxALIGN_CENTER, 5);
		return preview;
	}

	void ShowStick(wxInputDraw* preview, float deadzone, const glm::vec2& value)
	{
		preview->SetDeadzone(deadzone);
		preview->SetAxisValue(value);
	}
}

WiimoteControllerSettings::WiimoteControllerSettings(wxWindow* parent, const wxPoint& position, std::shared_ptr<NativeWiimoteController> controller)
	: wxDialog(parent, wxID_ANY, _("Controller settings"), position, wxDefaultSize, wxDEFAULT_DIALOG_STYLE),
	  m_controller(std::move(controller))
{
	m_settings = m_controller->get_settings();
	m_savedRumble = m_settings.rumble;
	m_savedPacketDelay = m_controller->get_packet_delay();
	SetSizeHints(wxDefaultSize, wxDefaultSize);

	auto* content = new wxBoxSizer(wxVERTICAL);
	auto addBox = [&](wxStaticBox* box, wxSizer* inner) {
		auto* boxSizer = new wxStaticBoxSizer(box, wxVERTICAL);
		boxSizer->Add(inner, 1, wxEXPAND, 0);
		content->Add(boxSizer, 0, wxALL | wxEXPAND, 5);
	};

	// extension currently reported by the remote
	{
		auto* box = new wxStaticBox(this, wxID_ANY, _("Connected extension"));
		auto* boxSizer = new wxStaticBoxSizer(box, wxVERTICAL);
		m_extensionName = new wxStaticText(box, wxID_ANY, _("None"));
		boxSizer->Add(m_extensionName, 0, wxALL, 5);
		content->Add(boxSizer, 0, wxALL | wxEXPAND, 5);
	}

	{
		auto* box = new wxStaticBox(this, wxID_ANY, _("Options"));
		auto* boxSizer = new wxStaticBoxSizer(box, wxVERTICAL);

		auto* toggles = new wxBoxSizer(wxHORIZONTAL);
		m_rumble = new wxCheckBox(box, wxID_ANY, _("Rumble"));
		m_rumble->SetValue(m_settings.rumble > 0);
		m_rumble->Bind(wxEVT_CHECKBOX, &WiimoteControllerSettings::OnRumbleToggled, this);
		toggles->Add(m_rumble, 0, wxALL, 5);
		m_motion = new wxCheckBox(box, wxID_ANY, _("Use motion"));
		m_motion->SetValue(m_settings.motion);
		m_motion->Enable(m_controller->has_motion());
		toggles->Add(m_motion, 0, wxALL, 5);
		boxSizer->Add(toggles);

		// read back on apply
		auto* delayRow = new wxBoxSizer(wxHORIZONTAL);
		m_packetDelay = AddSliderRow(box, delayRow, _("Packet delay"), m_savedPacketDelay, 1, 100, "%dms", {});
		boxSizer->Add(delayRow);

		content->Add(boxSizer, 0, wxALL | wxEXPAND, 5);
	}

	// the nunchuck stick and the classic left stick both use the axis setting
	{
		m_nunchuckBox = new wxStaticBox(this, wxID_ANY, _("Nunchuck"));
		auto* grid = new wxFlexGridSizer(0, 3, 0, 0);
		AddDeadzoneRow(m_nunchuckBox, grid, m_settings.axis);
		AddRangeRow(m_nunchuckBox, grid, m_settings.axis);
		grid->AddSpacer(1);
		m_nunchuckStick = AddPreview(m_nunchuckBox, grid);
		addBox(m_nunchuckBox, grid);
	}

	{
		m_classicBox = new wxStaticBox(this, wxID_ANY, _("Classic"));
		// left stick in the first three columns, right stick in the last three
		auto* grid = new wxFlexGridSizer(0, 6, 0, 0);
		AddDeadzoneRow(m_classicBox, grid, m_settings.axis);
		AddDeadzoneRow(m_classicBox, grid, m_settings.rotation);
		AddRangeRow(m_classicBox, grid, m_settings.axis);
		AddRangeRow(m_classicBox, grid, m_settings.rotation);
		grid->AddSpacer(1);
		m_classicLeftStick = AddPreview(m_classicBox, grid);
		grid->AddSpacer(1);
		grid->AddSpacer(1);
		m_classicRightStick = AddPreview(m_classicBox, grid);
		addBox(m_classicBox, grid);
	}

	{
		auto* buttons = new wxFlexGridSizer(0, 4, 0, 0);
		buttons->AddGrowableCol(3);

		auto* ok = new wxButton(this, wxID_ANY, _("OK"));
		ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			Apply();
			EndModal(wxID_OK);
		});
		buttons->Add(ok, 0, wxALL, 5);
		buttons->Add(0, 0, 0, wxEXPAND, 5);

		auto* cancel = new wxButton(this, wxID_ANY, _("Cancel"));
		cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
		buttons->Add(cancel, 0, wxALL, 5);

		auto* calibrate = new wxButton(this, wxID_ANY, _("Calibrate"));
		calibrate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_controller->calibrate(); });
		buttons->Add(calibrate, 0, wxALL | wxALIGN_RIGHT, 5);

		content->Add(buttons, 0, wxEXPAND, 5);
	}

	SetSizer(content);
	Layout();
	Fit();

	Bind(wxEVT_CLOSE_WINDOW, &WiimoteControllerSettings::OnCloseWindow, this);

	m_poll = new wxTimer(this);
	Bind(wxEVT_TIMER, &WiimoteControllerSettings::OnPoll, this);
	m_poll->Start(100);
}

WiimoteControllerSettings::~WiimoteControllerSettings()
{
	m_controller->stop_rumble();
	m_poll->Stop();
}

void WiimoteControllerSettings::Apply()
{
	m_controller->set_settings(m_settings);
	if (m_motion)
		m_controller->set_use_motion(m_motion->GetValue());
	m_controller->set_packet_delay(m_packetDelay->GetValue());
}

void WiimoteControllerSettings::OnPoll(wxTimerEvent& event)
{
	using namespace std::chrono;
	if (m_rumbleStarted && duration_cast<milliseconds>(steady_clock::now() - *m_rumbleStarted).count() > 500)
	{
		m_controller->stop_rumble();
		m_rumbleStarted.reset();
	}

	const auto& rest = m_controller->get_default_state();
	auto state = m_controller->raw_state();
	m_controller->apply_axis_setting(state.axis, rest.axis, m_settings.axis);
	m_controller->apply_axis_setting(state.rotation, rest.rotation, m_settings.rotation);
	m_controller->apply_axis_setting(state.trigger, rest.trigger, m_settings.trigger);

	if (m_nunchuckBox->IsEnabled())
		ShowStick(m_nunchuckStick, m_settings.axis.deadzone, state.axis);
	if (m_classicBox->IsEnabled())
	{
		ShowStick(m_classicLeftStick, m_settings.axis.deadzone, state.axis);
		ShowStick(m_classicRightStick, m_settings.rotation.deadzone, state.rotation);
	}

	ShowExtension();
}

void WiimoteControllerSettings::ShowExtension()
{
	const auto extension = m_controller->get_extension();
	const bool nunchuck = extension == NativeWiimoteController::Nunchuck;
	const bool classic = extension == NativeWiimoteController::Classic;
	m_nunchuckBox->Enable(nunchuck);
	m_classicBox->Enable(classic);

	wxString name;
	if (nunchuck)
		name = _("Nunchuck");
	else if (classic)
		name = _("Classic");

	if (m_controller->is_mpls_attached())
	{
		// "MotionPlus" alone, or "<extension> (MotionPlus)"
		if (name.empty())
			name = _("MotionPlus");
		else
			name << " (" << _("MotionPlus") << ")";
	}
	if (name.empty())
		name = _("None");
	m_extensionName->SetLabelText(name);
}

void WiimoteControllerSettings::OnCloseWindow(wxCloseEvent& event)
{
	// closing the window keeps the edits, like OK
	const int code = GetReturnCode();
	if (code == 0 || code == wxID_OK)
		Apply();
	event.Skip();
}

void WiimoteControllerSettings::OnRumbleToggled(wxCommandEvent& event)
{
	// short test buzz; the stored strength only changes on apply
	const bool on = m_rumble->GetValue();
	m_settings.rumble = on ? 1.0f : 0.0f;
	m_controller->set_rumble(m_settings.rumble);
	if (on)
		m_controller->start_rumble();
	else
		m_controller->stop_rumble();
	m_controller->set_rumble(m_savedRumble);
	m_rumbleStarted = std::chrono::steady_clock::now();
}

#endif
