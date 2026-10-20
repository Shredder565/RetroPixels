// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#ifdef SUPPORTS_WIIMOTE

#include <wx/dialog.h>
#include <wx/timer.h>
#include <wx/slider.h>

#include "input/api/Controller.h"
#include "input/api/Wiimote/NativeWiimoteController.h"

class wxStaticBox;
class wxStaticText;
class wxCheckBox;
class wxInputDraw;

// Settings for a real Wiimote: rumble, motion, packet delay and the stick
// tuning of whichever extension is plugged in (polled while open).
class WiimoteControllerSettings : public wxDialog
{
public:
	WiimoteControllerSettings(wxWindow* parent, const wxPoint& position, std::shared_ptr<NativeWiimoteController> controller);
	~WiimoteControllerSettings();

private:
	void Apply();
	void OnPoll(wxTimerEvent& event);
	void OnCloseWindow(wxCloseEvent& event);
	void OnRumbleToggled(wxCommandEvent& event);
	void ShowExtension();

	std::shared_ptr<NativeWiimoteController> m_controller;
	ControllerBase::Settings m_settings;
	float m_savedRumble;
	uint32 m_savedPacketDelay;

	wxTimer* m_poll;
	std::optional<std::chrono::steady_clock::time_point> m_rumbleStarted{};

	wxStaticText* m_extensionName;
	wxSlider* m_packetDelay;
	wxCheckBox* m_rumble = nullptr;
	wxCheckBox* m_motion = nullptr;

	wxStaticBox* m_nunchuckBox;
	wxInputDraw* m_nunchuckStick;

	wxStaticBox* m_classicBox;
	wxInputDraw* m_classicLeftStick;
	wxInputDraw* m_classicRightStick;
};

#endif
