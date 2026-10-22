// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/LoggingWindow.h"

#include "Cemu/Logging/CemuLogging.h"
#include "wxgui/helpers/wxLogEvent.h"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/wupdlock.h>

wxDEFINE_EVENT(EVT_LOG, wxLogEvent);

namespace
{
	// the category names the logger tags its lines with (see CemuLogging)
	const wxString kCategories[] = {
		"Unsupported APIs calls",
		"Coreinit Logging",
		"Coreinit File-Access",
		"Coreinit Thread-Synchronization",
		"Coreinit Memory",
		"Coreinit MP",
		"Coreinit Thread",
		"nn::nfp",
		"GX2",
		"Audio",
		"Input",
		"Socket",
		"Save",
		"H264",
		"Graphic pack patches",
		"Texture cache",
		"Texture readback",
		"OpenGL debug output",
		"Vulkan validation layer",
		"Metal debug output",
	};
}

LoggingWindow::LoggingWindow(wxFrame* parent)
	: wxFrame(parent, wxID_ANY, _("Logging window"), wxDefaultPosition, wxSize(800, 600), wxDEFAULT_FRAME_STYLE | wxTAB_TRAVERSAL)
{
	auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
	filterRow->Add(new wxStaticText(this, wxID_ANY, _("Filter")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	m_filter = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, (int)std::size(kCategories), kCategories);
	m_filter->Bind(wxEVT_COMBOBOX, &LoggingWindow::OnFilterChange, this);
	m_filter->Bind(wxEVT_TEXT, &LoggingWindow::OnFilterChange, this); // typed filters work too
	filterRow->Add(m_filter, 1, wxALL, 5);

	m_filter_message = new wxCheckBox(this, wxID_ANY, _("Filter messages"));
	m_filter_message->Bind(wxEVT_CHECKBOX, &LoggingWindow::OnFilterMessageChange, this);
	filterRow->Add(m_filter_message, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	m_log_list = new wxLogCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxScrolledWindowStyle, true);

	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(filterRow, 0, wxEXPAND, 5);
	layout->Add(m_log_list, 1, wxALL | wxEXPAND, 5);
	SetSizer(layout);
	Layout();

	Bind(EVT_LOG, &LoggingWindow::OnLogMessage, this);
	cemuLog_setCallbacks(this);
}

LoggingWindow::~LoggingWindow()
{
	Unbind(EVT_LOG, &LoggingWindow::OnLogMessage, this);
	cemuLog_clearCallbacks();
}

// both overloads hand the line to the list right away (on the logging thread)
void LoggingWindow::Log(std::string_view filter, std::string_view message)
{
	wxLogEvent line{std::string(filter), std::string(message)};
	OnLogMessage(line);
}

void LoggingWindow::Log(std::string_view filter, std::wstring_view message)
{
	wxLogEvent entry{std::string(filter), std::wstring(message)};
	OnLogMessage(entry); // same path as a message posted from another thread
}

void LoggingWindow::OnLogMessage(wxLogEvent& event)
{
	m_log_list->PushEntry(event.GetFilter(), event.GetMessage());
}

void LoggingWindow::OnFilterMessageChange(wxCommandEvent& event)
{
	event.Skip();
	m_log_list->SetFilterMessage(m_filter_message->GetValue());
}

void LoggingWindow::OnFilterChange(wxCommandEvent& event)
{
	event.Skip();
	m_log_list->SetActiveFilter(m_filter->GetValue().utf8_string());
}
