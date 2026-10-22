// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/components/wxInputDraw.h"

#include <wx/dcbuffer.h>
#include <wx/settings.h>

#include <glm/geometric.hpp>

namespace
{
	wxColour Sys(wxSystemColour c)
	{
		return wxSystemSettings::GetColour(c);
	}

	wxColour Faded(wxColour c)
	{
		c.MakeDisabled();
		return c;
	}

	// pen and brush colours for one state of the control
	struct StickInk
	{
		wxColour ring;                 // outer range circle
		wxColour zonePen, zoneFill;    // deadzone disc
		wxColour idlePen, idleFill;    // dot inside the deadzone
		wxColour activePen, activeFill; // dot outside the deadzone
		wxColour edgePen, edgeFill;    // dot at full deflection
	};

	StickInk MakeInk(bool enabled)
	{
		const wxColour accent = wxSystemSettings::SelectLightDark(0x336600, 0x99FF99);
		StickInk ink;
		ink.edgePen = ink.edgeFill = accent;
		if (enabled)
		{
			ink.ring = accent;
			ink.zonePen = ink.zoneFill = Sys(wxSYS_COLOUR_GRAYTEXT);
			ink.idlePen = ink.idleFill = Sys(wxSYS_COLOUR_WINDOWTEXT);
			ink.activePen = ink.activeFill = *wxRED;
		}
		else
		{
			ink.ring = Faded(Sys(wxSYS_COLOUR_GRAYTEXT));
			ink.zonePen = ink.zoneFill = Faded(Sys(wxSYS_COLOUR_GRAYTEXT));
			ink.idlePen = Sys(wxSYS_COLOUR_WINDOWTEXT);
			ink.idleFill = Sys(wxSYS_COLOUR_GRAYTEXT);
			ink.activePen = Sys(wxSYS_COLOUR_GRAYTEXT);
			ink.activeFill = Faded(Sys(wxSYS_COLOUR_GRAYTEXT));
		}
		return ink;
	}
}

wxInputDraw::wxInputDraw(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size)
	: wxWindow(parent, id, pos, size, 0, wxPanelNameStr)
{
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Bind(wxEVT_PAINT, &wxInputDraw::OnPaintEvent, this);
}

wxInputDraw::~wxInputDraw()
{
	Unbind(wxEVT_PAINT, &wxInputDraw::OnPaintEvent, this);
}

void wxInputDraw::OnPaintEvent(wxPaintEvent& /*event*/)
{
	wxAutoBufferedPaintDC dc(this);
	OnRender(dc);
}

void wxInputDraw::OnRender(wxDC& dc)
{
	const bool enabled = IsEnabled();
	const StickInk ink = MakeInk(enabled);
	// a disabled preview always shows the stick centred
	const glm::vec2 stick = enabled ? m_position : glm::vec2{};

	const wxColour window = Sys(wxSYS_COLOUR_WINDOW);
	dc.SetBackgroundMode(wxSOLID);
	dc.SetBackground(wxBrush(window));
	dc.SetPen(wxPen(window));
	dc.SetBrush(wxBrush(window));
	dc.Clear();

	const wxSize client = GetSize();
	const float side = (float)std::min(client.GetWidth(), client.GetHeight()) - 1.0f;
	const float half = side / 2.0f;
	const int cx = (int)half;

	dc.SetPen(wxPen(Sys(wxSYS_COLOUR_GRAYTEXT)));
	dc.DrawRectangle(wxRect(0, 0, (int)side, (int)side));

	dc.SetPen(wxPen(ink.ring));
	dc.DrawCircle(cx, cx, cx);

	wxColour dotPen = ink.activePen, dotFill = ink.activeFill;
	if (m_deadzone > 0)
	{
		dc.SetPen(wxPen(ink.zonePen));
		dc.SetBrush(wxBrush(ink.zoneFill));
		dc.DrawCircle(cx, cx, (int)(m_deadzone * side / 2.0f));

		const float deflection = glm::length(stick);
		if (deflection < m_deadzone)
		{
			dotPen = ink.idlePen;
			dotFill = ink.idleFill;
		}
		else if (std::abs(1.0f - deflection) < 0.05f)
		{
			dotPen = ink.edgePen;
			dotFill = ink.edgeFill;
		}
	}
	dc.SetPen(wxPen(dotPen));
	dc.SetBrush(wxBrush(dotFill));
	dc.DrawCircle((int)(half + stick.x * half), (int)(half - stick.y * half), 2);

	dc.SetBrush(*wxTRANSPARENT_BRUSH);
}

void wxInputDraw::SetAxisValue(const glm::vec2& position)
{
	m_position = position;
	Refresh();
}

void wxInputDraw::SetDeadzone(float deadzone)
{
	m_deadzone = deadzone;
	Refresh();
}
