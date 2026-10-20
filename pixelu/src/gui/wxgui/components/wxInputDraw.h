// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "input/api/ControllerState.h"
#include "util/math/vector2.h"

#include <wx/window.h>

// Small square preview of a stick: outer range circle, the deadzone (filled)
// and a dot at the current axis position.
class wxInputDraw : public wxWindow
{
public:
	wxInputDraw(wxWindow *parent, wxWindowID id, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
	~wxInputDraw();

	virtual void OnRender(wxDC& dc);
	void SetAxisValue(const glm::vec2& position);
	void SetDeadzone(float deadzone);

private:
	void OnPaintEvent(wxPaintEvent& event);

	glm::vec2 m_position{};
	float m_deadzone{0};
};
