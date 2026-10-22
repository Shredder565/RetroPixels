// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "wxgui/input/panels/InputPanel.h"
#include "input/emulated/WiimoteController.h"
#include <wx/slider.h>

class wxCheckBox;
class wxGridBagSizer;
class wxInputDraw;

// mapping page for an emulated Wiimote; the extension checkboxes pick the
// reported device type and enable the Nunchuck mappings
class WiimoteInputPanel : public InputPanel
{
public:
	WiimoteInputPanel(wxWindow* parent);

	void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller) override;

	void load_controller(const EmulatedControllerPtr& emulated_controller) override;

private:
	void ShowDeviceType(WPADDeviceType type);
	void OnExtensionToggled(wxCommandEvent& event);

	WPADDeviceType m_device_type = kWAPDevCore;

	wxCheckBox* m_motionPlus;
	wxCheckBox* m_nunchuck;
	wxCheckBox* m_classic; // kept hidden, classic is its own controller type
	wxInputDraw* m_nunchuckStick;
	std::vector<wxWindow*> m_nunchuckControls;
};
