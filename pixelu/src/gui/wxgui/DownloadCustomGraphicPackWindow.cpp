// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/DownloadCustomGraphicPackWindow.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"

#include <cstring>
#include <filesystem>
#include <string>

#include <wx/event.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/gdicmn.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/timer.h>

#include <curl/curl.h>
#include <zip.h>

namespace
{
	using DownloadState = DownloadCustomGraphicPackWindow::curlDownloadFileState_t;

	constexpr uint64 kMaxFileSize = 1000000000; // anything bigger in a graphic pack is suspicious

	size_t ReceiveData(void* data, size_t size, size_t count, DownloadState* state)
	{
		const auto* bytes = static_cast<const uint8*>(data);
		state->fileData.insert(state->fileData.end(), bytes, bytes + size * count);
		return size * count;
	}

	int ReportProgress(DownloadState* state, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t)
	{
		if (state->isCanceled)
			return 1; // aborts the transfer
		state->progress = total > 1 ? (double)now / (double)total : 0.0;
		return 0;
	}

	bool Download(const char* url, DownloadState* state)
	{
		CURL* curl = curl_easy_init();
		if (!curl)
			return false;
		state->progress = 0.0;
		state->fileData.clear();
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ReceiveData);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, state);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ReportProgress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, state);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		// give up on dead or stalled servers
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 30L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);
		const CURLcode result = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		return result == CURLE_OK;
	}

	// the pack's folder name: the URL's last path part without extension ("NewCustomPack" if that is empty)
	std::string PackFolderName(wxString url)
	{
		const int slash = url.Find('/', true);
		wxString name = slash == wxNOT_FOUND ? url : url.Mid(slash + 1);
		const int dot = name.Find('.', true);
		if (dot != wxNOT_FOUND)
			name = name.Left(dot);
		name.Trim(true).Trim(false);
		return name.IsEmpty() ? std::string("NewCustomPack") : name.ToStdString();
	}

	// rejects anything that could land outside the pack folder
	bool IsUnsafeEntryName(const std::string& name)
	{
		return name.find("../") != std::string::npos || name.find("..\\") != std::string::npos ||
			   (!name.empty() && (name.front() == '/' || name.front() == '\\'));
	}
}

DownloadCustomGraphicPackWindow::DownloadCustomGraphicPackWindow(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Download Graphic Pack from URL"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_stage(StageDone), m_currentStage(StageDone)
{
	auto* layout = new wxBoxSizer(wxVERTICAL);

	m_urlField = new wxTextCtrl(this, wxID_ANY, wxEmptyString);
	m_urlField->SetHint(_("Enter download URL..."));
	layout->Add(m_urlField, 0, wxALL | wxEXPAND, 5);

	m_processBar = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(500, 20), wxGA_HORIZONTAL);
	m_processBar->SetValue(0);
	m_processBar->SetRange(100);
	layout->Add(m_processBar, 0, wxALL | wxEXPAND, 5);

	// status on the left, buttons on the right
	auto* bottom = new wxBoxSizer(wxHORIZONTAL);
	m_statusText = new wxStaticText(this, wxID_ANY, _("Ready..."));
	bottom->Add(m_statusText, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
	bottom->AddStretchSpacer(1);

	auto* close = new wxButton(this, wxID_ANY, _("Close"));
	close->Bind(wxEVT_BUTTON, &DownloadCustomGraphicPackWindow::OnCancelButton, this);
	bottom->Add(close, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	m_downloadButton = new wxButton(this, wxID_ANY, _("Download"));
	m_downloadButton->Bind(wxEVT_BUTTON, &DownloadCustomGraphicPackWindow::OnDownloadButton, this);
	bottom->Add(m_downloadButton, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	layout->Add(bottom, 0, wxEXPAND | wxALL, 5);

	SetSizer(layout);
	Centre(wxBOTH);
	Layout();
	Fit();

	m_timer = new wxTimer(this);
	Bind(wxEVT_TIMER, &DownloadCustomGraphicPackWindow::OnUpdate, this);
	Bind(wxEVT_CLOSE_WINDOW, &DownloadCustomGraphicPackWindow::OnClose, this);
	m_timer->Start(100);

	m_downloadState = std::make_unique<curlDownloadFileState_t>();
}

DownloadCustomGraphicPackWindow::~DownloadCustomGraphicPackWindow()
{
	if (m_downloadState)
		m_downloadState->isCanceled = true;
	m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();
}

int DownloadCustomGraphicPackWindow::ShowModal()
{
	wxDialog::ShowModal();
	return wxID_OK;
}

void DownloadCustomGraphicPackWindow::OnClose(wxCloseEvent& event)
{
	event.Skip(); // let the dialog close once the download has stopped
	m_timer->Stop();
	if (m_downloadState)
		m_downloadState->isCanceled = true;
	if (m_thread.joinable())
		m_thread.join();
}

void DownloadCustomGraphicPackWindow::OnUpdate(const wxTimerEvent&)
{
	// input is locked while a download runs
	const bool idle = m_currentStage >= StageDone;
	m_downloadButton->Enable(idle);
	m_urlField->Enable(idle);

	if (m_currentStage == StageDownloading || m_currentStage == StageExtracting)
	{
		const double fraction = m_currentStage == StageDownloading ? m_downloadState->progress.load() : m_extractionProgress.load();
		const sint32 percent = (sint32)(fraction * 100.0f);
		if (m_processBar->GetValue() != percent)
			m_processBar->SetValue(percent);
	}

	const DownloadStage_t stage = m_stage;
	if (stage == m_currentStage)
		return;

	wxString status = "...";
	wxColour colour = *wxWHITE;
	auto failed = [&](const char* text) {
		status = text;
		colour = *wxRED;
		m_processBar->SetValue(0);
	};
	switch (stage)
	{
	case StageDownloading: status = "Downloading..."; break;
	case StageVerifying: status = "Verifying..."; break;
	case StageExtracting: status = "Extracting..."; break;
	case StageDone:
		status = "Done!";
		colour = *wxGREEN;
		m_processBar->SetValue(100);
		break;
	case StageErrConnectFailed:
		if (m_urlField->GetValue().empty())
		{
			status = "Please enter a valid URL.";
			m_processBar->SetValue(0);
		}
		else
			failed("ERROR: Connection failed.");
		break;
	case StageErrInvalidPack: failed("ERROR: Invalid pack."); break;
	case StageErrSourceFailed: failed("ERROR: Failed to create ZIP source."); break;
	case StageErrOpenFailed: failed("ERROR: Failed to open downloaded ZIP."); break;
	case StageErrConflict: failed("ERROR: File conflict. Pack already installed?"); break;
	}
	m_currentStage = stage;
	m_statusText->SetLabel(status);
	m_statusText->SetForegroundColour(colour);
}

void DownloadCustomGraphicPackWindow::OnCancelButton(const wxCommandEvent&)
{
	Close();
}

void DownloadCustomGraphicPackWindow::OnDownloadButton(const wxCommandEvent&)
{
	m_downloadButton->Disable();
	m_urlField->Disable();
	if (m_thread.joinable())
		m_thread.join();
	m_thread = std::thread(&DownloadCustomGraphicPackWindow::UpdateThread, this);
}

void DownloadCustomGraphicPackWindow::UpdateThread()
{
	const wxString url = m_urlField->GetValue();

	m_stage = StageDownloading;
	if (!Download(url.utf8_str(), m_downloadState.get()))
	{
		m_stage = StageErrConnectFailed;
		return;
	}

	m_extractionProgress = 0.0;
	m_stage = StageExtracting;

	zip_error_t error;
	zip_error_init(&error);
	zip_source_t* source = zip_source_buffer_create(m_downloadState->fileData.data(), m_downloadState->fileData.size(), 0, &error);
	if (!source)
	{
		zip_error_fini(&error);
		m_stage = StageErrSourceFailed;
		return;
	}
	zip_t* zip = zip_open_from_source(source, 0, &error);
	if (!zip)
	{
		zip_source_free(source);
		zip_error_fini(&error);
		m_stage = StageErrOpenFailed;
		return;
	}

	// returns the final stage; StageExtracting when cancelled (the window is closing, nothing to show)
	auto unpack = [&]() -> DownloadStage_t {
		// a graphic pack has its rules.txt at the top of the archive
		zip_stat_t rules;
		zip_stat_init(&rules);
		if (zip_stat(zip, "rules.txt", 0, &rules) != 0)
			return StageErrInvalidPack;

		const fs::path packFolder = ActiveSettings::GetUserDataPath("graphicPacks/customGraphicPacks") / PackFolderName(url);
		fs::create_directories(packFolder);

		const zip_int64_t count = zip_get_num_entries(zip, 0);
		for (zip_int64_t i = 0; i < count; ++i)
		{
			m_extractionProgress = (double)i / (double)count;
			if (m_downloadState->isCanceled)
				return StageExtracting;

			zip_stat_t info;
			zip_stat_init(&info);
			if (zip_stat_index(zip, i, 0, &info) != 0 || !info.name)
				continue;
			const std::string name = info.name;
			if (IsUnsafeEntryName(name))
				return StageErrInvalidPack; // downloaded from anywhere: refuse instead of skipping

			const fs::path target = packFolder / name;
			if (!name.empty() && name.back() == '/')
			{
				fs::create_directories(target);
				continue;
			}
			if (fs::exists(target) && fs::is_regular_file(target))
				return StageErrConflict;
			fs::create_directories(target.parent_path());

			zip_file_t* entry = zip_fopen_index(zip, i, 0);
			if (!entry)
				continue;
			if (info.size > kMaxFileSize)
			{
				zip_fclose(entry);
				return StageErrInvalidPack;
			}
			std::vector<uint8> data(info.size);
			const bool complete = zip_fread(entry, data.data(), info.size) == (zip_int64_t)info.size;
			zip_fclose(entry);
			if (!complete)
				return StageErrInvalidPack;
			std::unique_ptr<FileStream> out(FileStream::createFile2(target));
			if (out)
				out->writeData(data.data(), data.size());
		}
		return StageDone;
	};

	DownloadStage_t result;
	try
	{
		result = unpack();
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Custom graphic pack: {}", ex.what());
		result = StageErrOpenFailed;
	}
	zip_discard(zip);
	zip_error_fini(&error);

	if (result != StageExtracting)
		m_stage = result;
}
