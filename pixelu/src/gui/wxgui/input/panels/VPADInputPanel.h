// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/gbsizer.h>
#include "wxgui/input/panels/InputPanel.h"

class wxInputDraw;
class wxCheckBox;

// mapping page for the emulated Wii U GamePad
class VPADInputPanel : public InputPanel
{
public:
	VPADInputPanel(wxWindow* parent);

	void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller) override;
	virtual void load_controller(const EmulatedControllerPtr& controller) override;

private:
	wxInputDraw* m_leftStick;
	wxInputDraw* m_rightStick;
	wxCheckBox* m_screenToggle; // "show screen" toggles instead of holding
};
