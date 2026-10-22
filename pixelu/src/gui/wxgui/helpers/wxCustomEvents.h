// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/event.h>
#include <wx/gauge.h>

class wxStatusBar;
class wxControl;

wxDECLARE_EVENT(wxEVT_REMOVE_ITEM, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_SET_TEXT, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_ENABLE, wxCommandEvent);

// Asks a frame to put `text` into status bar field `number`.
class wxSetStatusBarTextEvent;
wxDECLARE_EVENT(wxEVT_SET_STATUS_BAR_TEXT, wxSetStatusBarTextEvent);
class wxSetStatusBarTextEvent : public wxCommandEvent
{
public:
	wxSetStatusBarTextEvent(const wxString& text, int number = 0, wxEventType type = wxEVT_SET_STATUS_BAR_TEXT, int id = 0)
		: wxCommandEvent(type, id), m_label(text), m_field(number)
	{
	}
	wxSetStatusBarTextEvent(const wxSetStatusBarTextEvent& other)
		: wxCommandEvent(other), m_label(other.GetText()), m_field(other.GetNumber())
	{
	}

	[[nodiscard]] const wxString& GetText() const { return m_label; }
	[[nodiscard]] int GetNumber() const { return m_field; }

	wxEvent* Clone() const override { return new wxSetStatusBarTextEvent(*this); }

private:
	const wxString m_label;
	const int m_field;
};

// Asks the receiver to move a gauge (the event object) to `value`, optionally
// changing its range (when non-zero) and the label of `text_ctrl`.
class wxSetGaugeValue;
wxDECLARE_EVENT(wxEVT_SET_GAUGE_VALUE, wxSetGaugeValue);
class wxSetGaugeValue : public wxCommandEvent
{
public:
	wxSetGaugeValue(int value, wxGauge* gauge, wxControl* text_ctrl = nullptr, const wxString& text = wxEmptyString, wxEventType type = wxEVT_SET_GAUGE_VALUE, int id = 0)
		: wxSetGaugeValue(value, 0, gauge, text_ctrl, text, type, id)
	{
	}
	wxSetGaugeValue(int value, int range, wxGauge* gauge, wxControl* text_ctrl = nullptr, const wxString& text = wxEmptyString, wxEventType type = wxEVT_SET_GAUGE_VALUE, int id = 0)
		: wxCommandEvent(type, id), m_pos(value), m_max(range), m_label(text_ctrl), m_labelText(text)
	{
		SetEventObject(gauge);
	}
	wxSetGaugeValue(const wxSetGaugeValue& other)
		: wxCommandEvent(other), m_pos(other.GetValue()), m_max(other.GetRange()), m_label(other.GetTextCtrl()), m_labelText(other.GetText())
	{
	}

	[[nodiscard]] int GetValue() const { return m_pos; }
	[[nodiscard]] int GetRange() const { return m_max; }
	[[nodiscard]] wxGauge* GetGauge() const { return static_cast<wxGauge*>(GetEventObject()); }
	[[nodiscard]] const wxString& GetText() const { return m_labelText; }
	[[nodiscard]] wxControl* GetTextCtrl() const { return m_label; }

	wxEvent* Clone() const override { return new wxSetGaugeValue(*this); }

private:
	const int m_pos;
	const int m_max;
	wxControl* m_label;
	const wxString m_labelText;
};
