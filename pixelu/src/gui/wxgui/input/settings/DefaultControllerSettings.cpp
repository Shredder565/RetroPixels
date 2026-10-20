// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/settings/DefaultControllerSettings.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include "wxgui/components/wxInputDraw.h"

namespace
{
	// "caption  [slider]  value" across three cells of a sizer; the value label
	// follows the slider and onChange gets every new position
	wxSlider* AddSliderRow(wxWindow* parent, wxSizer* sizer, const wxString& caption, int value, int min, int max,
						   std::function<void(int)> onChange)
	{
		sizer->Add(new wxStaticText(parent, wxID_ANY, caption), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		auto* slider = new wxSlider(parent, wxID_ANY, value, min, max);
		auto* valueText = new wxStaticText(parent, wxID_ANY, wxString::Format("%d%%", value));
		sizer->Add(slider, 1, wxALL | wxEXPAND, 5);
		sizer->Add(valueText, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		slider->Bind(wxEVT_SLIDER, [slider, valueText, onChange = std::move(onChange)](wxCommandEvent&) {
			valueText->SetLabel(wxString::Format("%d%%", slider->GetValue()));
			onChange(slider->GetValue());
		});
		return slider;
	}

	int ToPercent(float v)
	{
		return (int)(v * 100);
	}

	// deadzone / range box for one analog input, with a live preview
	wxStaticBoxSizer* AddAxisBox(wxWindow* parent, const wxString& title, ControllerBase::AxisSetting& setting, wxInputDraw*& preview)
	{
		auto* box = new wxStaticBox(parent, wxID_ANY, title);
		auto* grid = new wxFlexGridSizer(0, 3, 0, 0);
		AddSliderRow(box, grid, _("Deadzone"), ToPercent(setting.deadzone), 0, 100, [&setting](int v) { setting.deadzone = (float)v / 100.0f; });
		AddSliderRow(box, grid, _("Range"), ToPercent(setting.range), 50, 200, [&setting](int v) { setting.range = (float)v / 100.0f; });
		grid->AddSpacer(1);
		preview = new wxInputDraw(box, wxID_ANY, wxDefaultPosition, {60, 60});
		grid->Add(preview, 0, wxTOP | wxBOTTOM | wxALIGN_CENTER, 5);

		auto* boxSizer = new wxStaticBoxSizer(box, wxVERTICAL);
		boxSizer->Add(grid, 1, wxEXPAND, 0);
		return boxSizer;
	}
}

DefaultControllerSettings::DefaultControllerSettings(wxWindow* parent, const wxPoint& position, std::shared_ptr<ControllerBase> controller)
	: wxDialog(parent, wxID_ANY, _("Controller settings"), position, wxDefaultSize, wxDEFAULT_DIALOG_STYLE),
	  m_controller(std::move(controller))
{
	m_settings = m_controller->get_settings();
	m_savedRumble = m_settings.rumble;
	SetSizeHints(wxDefaultSize, wxDefaultSize);

	auto* content = new wxBoxSizer(wxVERTICAL);

	{
		auto* box = new wxStaticBox(this, wxID_ANY, _("Options"));
		auto* boxSizer = new wxStaticBoxSizer(box, wxVERTICAL);

		m_motion = new wxCheckBox(box, wxID_ANY, _("Use motion"));
		m_motion->SetValue(m_settings.motion);
		m_motion->Enable(m_controller->has_motion());
		boxSizer->Add(m_motion, 0, wxEXPAND | wxALL, 5);

		auto* rumbleRow = new wxBoxSizer(wxHORIZONTAL);
		AddSliderRow(box, rumbleRow, _("Rumble"), ToPercent(m_settings.rumble), 0, 100, [this](int v) { PreviewRumble(v); });
		boxSizer->Add(rumbleRow);

		content->Add(boxSizer, 1, wxALL | wxEXPAND, 5);
	}

	auto* analogRow = new wxBoxSizer(wxHORIZONTAL);
	analogRow->Add(AddAxisBox(this, _("Axis"), m_settings.axis, m_axisPreview), 0, wxALL | wxEXPAND, 5);
	analogRow->Add(AddAxisBox(this, _("Rotation"), m_settings.rotation, m_rotationPreview), 0, wxALL | wxEXPAND, 5);
	analogRow->Add(AddAxisBox(this, _("Trigger"), m_settings.trigger, m_triggerPreview), 0, wxALL | wxEXPAND, 5);
	content->Add(analogRow);

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

	Bind(wxEVT_CLOSE_WINDOW, &DefaultControllerSettings::OnCloseWindow, this);

	m_poll = new wxTimer(this);
	Bind(wxEVT_TIMER, &DefaultControllerSettings::OnPoll, this);
	m_poll->Start(100);
}

DefaultControllerSettings::~DefaultControllerSettings()
{
	m_controller->stop_rumble();
	m_poll->Stop();
}

void DefaultControllerSettings::Apply()
{
	m_controller->set_settings(m_settings);
	if (m_motion)
		m_controller->set_use_motion(m_motion->GetValue());
}

void DefaultControllerSettings::PreviewRumble(int percent)
{
	// buzz once at the new strength, but keep the stored strength until applied
	m_settings.rumble = (float)percent / 100.0f;
	m_controller->set_rumble(m_settings.rumble);
	if (percent != 0)
		m_controller->start_rumble();
	else
		m_controller->stop_rumble();
	m_controller->set_rumble(m_savedRumble);
	m_rumbleStarted = std::chrono::steady_clock::now();
}

void DefaultControllerSettings::OnPoll(wxTimerEvent& event)
{
	using namespace std::chrono;
	if (m_rumbleStarted && duration_cast<milliseconds>(steady_clock::now() - *m_rumbleStarted).count() > 500)
	{
		m_controller->stop_rumble();
		m_rumbleStarted.reset();
	}

	// preview what the current (unsaved) settings would produce
	const auto& rest = m_controller->get_default_state();
	auto state = m_controller->raw_state();
	m_controller->apply_axis_setting(state.axis, rest.axis, m_settings.axis);
	m_controller->apply_axis_setting(state.rotation, rest.rotation, m_settings.rotation);
	m_controller->apply_axis_setting(state.trigger, rest.trigger, m_settings.trigger);
	if (m_controller->api() == InputAPI::SDLController)
	{
		// SDL reports up as negative
		state.axis.y = -state.axis.y;
		state.rotation.y = -state.rotation.y;
	}

	m_axisPreview->SetDeadzone(m_settings.axis.deadzone);
	m_axisPreview->SetAxisValue(state.axis);
	m_rotationPreview->SetDeadzone(m_settings.rotation.deadzone);
	m_rotationPreview->SetAxisValue(state.rotation);
	m_triggerPreview->SetDeadzone(m_settings.trigger.deadzone);
	m_triggerPreview->SetAxisValue(state.trigger);
}

void DefaultControllerSettings::OnCloseWindow(wxCloseEvent& event)
{
	// closing the window keeps the edits, like OK
	const int code = GetReturnCode();
	if (code == 0 || code == wxID_OK)
		Apply();
	event.Skip();
}
