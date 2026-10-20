// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/event.h>

// One log line for the logging window: the filter (log type) it belongs to and its text.
class wxLogEvent;
wxDECLARE_EVENT(EVT_LOG, wxLogEvent);

class wxLogEvent : public wxCommandEvent
{
public:
	wxLogEvent(const wxString& filter, const wxString& message)
		: wxCommandEvent(EVT_LOG), m_category(filter), m_text(message)
	{
	}
	wxLogEvent(const wxLogEvent& other)
		: wxCommandEvent(other), m_category(other.GetFilter()), m_text(other.GetMessage())
	{
	}

	wxEvent* Clone() const { return new wxLogEvent(*this); }

	[[nodiscard]] const wxString& GetFilter() const { return m_category; }
	[[nodiscard]] const wxString& GetMessage() const { return m_text; }

private:
	wxString m_category;
	wxString m_text;
};
