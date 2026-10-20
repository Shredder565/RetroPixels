// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/gbsizer.h>
#include "input/emulated/ProController.h"
#include "wxgui/input/panels/InputPanel.h"
#include "wxgui/components/wxInputDraw.h"

// mapping page for an emulated Wii U Pro Controller
class ProControllerInputPanel : public InputPanel
{
public:
	ProControllerInputPanel(wxWindow* parent);

	void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller) override;

private:
	wxInputDraw* m_leftStick;
	wxInputDraw* m_rightStick;
};
