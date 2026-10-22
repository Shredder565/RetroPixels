// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/dialog.h>
#include <wx/timer.h>
#include <wx/slider.h>

#include "input/api/Controller.h"

class wxCheckBox;
class wxInputDraw;

// Per-controller tuning: deadzone and range of the sticks/triggers, rumble
// strength and motion. Edits stay local until OK (or closing the window).
class DefaultControllerSettings : public wxDialog
{
public:
	DefaultControllerSettings(wxWindow* parent, const wxPoint& position, ControllerPtr controller);
	~DefaultControllerSettings();

private:
	void Apply();
	void OnPoll(wxTimerEvent& event);
	void OnCloseWindow(wxCloseEvent& event);
	void PreviewRumble(int percent);

	ControllerPtr m_controller;
	ControllerBase::Settings m_settings;
	float m_savedRumble;

	wxTimer* m_poll;
	std::optional<std::chrono::steady_clock::time_point> m_rumbleStarted{};

	wxCheckBox* m_motion = nullptr;
	wxInputDraw* m_axisPreview;
	wxInputDraw* m_rotationPreview;
	wxInputDraw* m_triggerPreview;
};
