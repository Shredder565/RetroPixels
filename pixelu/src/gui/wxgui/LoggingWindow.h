// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cemu/Logging/CemuLogging.h"

#include <wx/frame.h>
#include <wx/listbox.h>
#include <wx/combobox.h>
#include "wxgui/components/wxLogCtrl.h"

class wxLogEvent;

// Debug > Logging window: shows log lines as they are written, optionally only those of one category
// (and with "Filter messages", only lines containing the filter text). Receives the log through the
// logging callbacks while it is open.
class LoggingWindow : public wxFrame, public LoggingCallbacks
{
  public:
	LoggingWindow(wxFrame* parent);
	~LoggingWindow();

	void Log(std::string_view filter, std::string_view message) override;
	void Log(std::string_view filter, std::wstring_view message) override;

  private:
	void OnLogMessage(wxLogEvent& event);
	void OnFilterChange(wxCommandEvent& event);
	void OnFilterMessageChange(wxCommandEvent& event);

	wxComboBox* m_filter;
	wxLogCtrl* m_log_list;
	wxCheckBox* m_filter_message;
};
