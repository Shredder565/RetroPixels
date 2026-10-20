// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/gbsizer.h>
#include "input/emulated/ClassicController.h"
#include "wxgui/input/panels/InputPanel.h"

class wxInputDraw;

// mapping page for an emulated Classic Controller
class ClassicControllerInputPanel : public InputPanel
{
public:
	ClassicControllerInputPanel(wxWindow* parent);

	void on_timer(const EmulatedControllerPtr& emulated_controller, const ControllerPtr& controller) override;

private:
	wxInputDraw* m_leftStick;
	wxInputDraw* m_rightStick;
};
