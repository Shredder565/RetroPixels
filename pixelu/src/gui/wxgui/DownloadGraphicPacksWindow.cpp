// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/DownloadGraphicPacksWindow.h"

#include <filesystem>
#include <curl/curl.h>
#include <zip.h>
#include <rapidjson/document.h>
#include <boost/algorithm/string.hpp>

#include "config/ActiveSettings.h"
#include "Common/FileStream.h"

#include "Cafe/CafeSystem.h"

struct DownloadGraphicPacksWindow::curlDownloadFileState_t
{
	std::vector<uint8> fileData;
	double progress; // 0..1
};

namespace
{
	// asked first; it names the release feed for this emulator version
	constexpr const char* kQueryUrl = "https://cemu.info/api2/query_graphicpack_url.php";
	// used when the query fails
	constexpr const char* kDefaultReleaseUrl = "https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest";
	constexpr uint64 kMaxPackFileSize = 128ull * 1024 * 1024;

	fs::path DownloadFolder()
	{
		return ActiveSettings::GetUserDataPath("graphicPacks/downloadedGraphicPacks");
	}

	fs::path VersionFile()
	{
		return ActiveSettings::GetUserDataPath("graphicPacks/downloadedGraphicPacks/version.txt");
	}

	// whether the installed packs are the given release (first line of version.txt)
	bool IsInstalledRelease(const char* release)
	{
		std::unique_ptr<FileStream> file(FileStream::openFile2(VersionFile()));
		std::string installed;
		return file && file->readLine(installed) && boost::iequals(installed, release);
	}

	void WriteInstalledRelease(const char* release)
	{
		std::unique_ptr<FileStream> file(FileStream::createFile2(VersionFile()));
		if (file)
			file->writeString(release);
		else
			cemuLog_log(LogType::Force, "Failed to write graphic pack version.txt");
	}

	// empties the download folder (the folder itself stays; recreating it right away is unreliable)
	void ClearDownloadFolder()
	{
		const fs::path folder = DownloadFolder();
		std::error_code ec;
		if (!fs::exists(folder, ec))
			return;
		try
		{
			for (const auto& entry : fs::directory_iterator(folder))
				fs::remove_all(entry.path(), ec);
		}
		catch (const fs::filesystem_error& e)
		{
			cemuLog_log(LogType::Force, "Error in deleteDownloadedGraphicPacks():");
			cemuLog_log(LogType::Force, e.what());
		}
	}

	// the release name (it includes the version) and its first asset's download link
	bool ReadRelease(const std::vector<uint8>& reply, std::string& name, std::string& downloadUrl)
	{
		rapidjson::Document doc;
		doc.Parse((const char*)reply.data(), reply.size());
		if (doc.HasParseError() || !doc.IsObject())
			return false;
		if (!doc.HasMember("name") || !doc["name"].IsString())
			return false;
		if (!doc.HasMember("assets") || !doc["assets"].IsArray() || doc["assets"].Empty())
			return false;
		const auto& asset = doc["assets"][0];
		if (!asset.IsObject() || !asset.HasMember("browser_download_url") || !asset["browser_download_url"].IsString())
			return false;
		name = doc["name"].GetString();
		downloadUrl = asset["browser_download_url"].GetString();
		return true;
	}
}

size_t DownloadGraphicPacksWindow::curlDownloadFile_writeData(void* ptr, size_t size, size_t nmemb, curlDownloadFileState_t* downloadState)
{
	const size_t bytes = size * nmemb;
	const auto* data = static_cast<const uint8*>(ptr);
	downloadState->fileData.insert(downloadState->fileData.end(), data, data + bytes);
	return bytes;
}

int DownloadGraphicPacksWindow::progress_callback(curlDownloadFileState_t* downloadState, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
{
	downloadState->progress = dltotal > 1 ? (double)dlnow / (double)dltotal : 0.0;
	return 0;
}

bool DownloadGraphicPacksWindow::curlDownloadFile(const char* url, curlDownloadFileState_t* downloadState)
{
	CURL* curl = curl_easy_init();
	if (!curl)
		return false;

	downloadState->progress = 0.0;
	downloadState->fileData.clear();
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlDownloadFile_writeData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, downloadState);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, downloadState);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);
	const CURLcode result = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	return result == CURLE_OK;
}

void DownloadGraphicPacksWindow::UpdateThread()
{
	auto fail = [this] { m_threadState = ThreadError; };
	auto connectionError = [this] {
		wxMessageBox(_(L"Failed to connect to server"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
	};

	// 1. which release feed to read (the time parameter defeats caching proxies)
	curlDownloadFileState_t reply;
	std::string releaseUrl;
	const std::string query = fmt::format("{}?version={}.{}.{}&t={}", kQueryUrl, EMULATOR_VERSION_MAJOR, EMULATOR_VERSION_MINOR, EMULATOR_VERSION_PATCH, (uint32)std::time(nullptr));
	if (curlDownloadFile(query.c_str(), &reply))
		releaseUrl.assign(reply.fileData.cbegin(), reply.fileData.cend());
	if (releaseUrl.empty())
	{
		cemuLog_log(LogType::Force, "Graphic pack update request failed or returned invalid URL. Using default repository URL instead");
		releaseUrl = kDefaultReleaseUrl;
	}

	// 2. the latest release
	if (!curlDownloadFile(releaseUrl.c_str(), &reply))
	{
		connectionError();
		fail();
		return;
	}
	std::string releaseName, archiveUrl;
	if (!ReadRelease(reply.fileData, releaseName, archiveUrl))
	{
		fail();
		return;
	}
	if (IsInstalledRelease(releaseName.c_str()))
	{
		wxMessageBox(_("No updates available."), _("Graphic packs"), wxOK | wxCENTRE, this);
		m_threadState = ThreadFinished;
		return;
	}

	// 3. download the archive
	m_stage = StageDownloading;
	if (!curlDownloadFile(archiveUrl.c_str(), m_downloadState.get()))
	{
		connectionError();
		fail();
		return;
	}

	// 4. replace the downloaded packs with its contents
	m_extractionProgress = 0.0;
	m_stage = StageExtracting;

	zip_error_t error;
	zip_error_init(&error);
	zip_source_t* source = zip_source_buffer_create(m_downloadState->fileData.data(), m_downloadState->fileData.size(), 0, &error);
	if (!source)
	{
		zip_error_fini(&error);
		fail();
		return;
	}
	zip_t* zip = zip_open_from_source(source, 0, &error);
	if (!zip)
	{
		zip_source_free(source);
		zip_error_fini(&error);
		fail();
		return;
	}

	ClearDownloadFolder();
	std::error_code ec;
	fs::create_directories(DownloadFolder(), ec);

	const sint32 count = (sint32)zip_get_num_entries(zip, 0);
	for (sint32 i = 0; i < count; ++i)
	{
		m_extractionProgress = (double)i / (double)count;
		zip_stat_t info{};
		if (zip_stat_index(zip, i, 0, &info) != 0 || !info.name)
			continue;
		const std::string_view name = info.name;
		if (name.empty() || name.find("../") != std::string_view::npos || name.find("..\\") != std::string_view::npos)
			continue; // never write outside the folder

		const fs::path target = ActiveSettings::GetUserDataPath("graphicPacks/downloadedGraphicPacks/{}", info.name);
		if (name.back() == '/')
		{
			fs::create_directories(target, ec);
			continue;
		}
		if (info.size == 0 || info.size > kMaxPackFileSize)
			continue;

		zip_file_t* entry = zip_fopen_index(zip, i, 0);
		if (!entry)
			continue;
		std::vector<uint8> data(info.size);
		if (zip_fread(entry, data.data(), info.size) == (zip_int64_t)info.size)
		{
			std::unique_ptr<FileStream> out(FileStream::createFile2(target));
			if (out)
				out->writeData(data.data(), data.size());
		}
		zip_fclose(entry);
	}
	zip_discard(zip);
	zip_error_fini(&error);

	WriteInstalledRelease(releaseName.c_str());
	m_threadState = ThreadFinished;
}

DownloadGraphicPacksWindow::DownloadGraphicPacksWindow(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Checking version..."), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_threadState(ThreadRunning), m_stage(StageCheckVersion), m_currentStage(StageCheckVersion)
{
	auto* layout = new wxBoxSizer(wxVERTICAL);

	m_processBar = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(500, 20), wxGA_HORIZONTAL);
	m_processBar->SetValue(0);
	m_processBar->SetRange(100);
	layout->Add(m_processBar, 0, wxALL | wxEXPAND, 5);

	auto* cancel = new wxButton(this, wxID_ANY, _("Cancel"));
	cancel->Bind(wxEVT_BUTTON, &DownloadGraphicPacksWindow::OnCancelButton, this);
	layout->Add(cancel, 0, wxALIGN_RIGHT | wxALL, 5);

	SetSizer(layout);
	Centre(wxBOTH);
	Layout();
	Fit();

	m_timer = new wxTimer(this);
	Bind(wxEVT_TIMER, &DownloadGraphicPacksWindow::OnUpdate, this);
	Bind(wxEVT_CLOSE_WINDOW, &DownloadGraphicPacksWindow::OnClose, this);
	m_timer->Start(250);

	m_downloadState = std::make_unique<curlDownloadFileState_t>();
}

DownloadGraphicPacksWindow::~DownloadGraphicPacksWindow()
{
	m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();
}

const std::string& DownloadGraphicPacksWindow::GetException() const
{
	return m_threadException;
}

int DownloadGraphicPacksWindow::ShowModal()
{
	if (CafeSystem::IsTitleRunning())
	{
		wxMessageBox(_("Graphic packs cannot be updated while a game is running."), _("Graphic packs"), wxOK | wxCENTRE, this);
		return wxID_CANCEL;
	}
	m_thread = std::thread(&DownloadGraphicPacksWindow::UpdateThread, this);
	wxDialog::ShowModal();
	return m_threadState == ThreadCanceled ? wxID_CANCEL : wxID_OK;
}

void DownloadGraphicPacksWindow::OnClose(wxCloseEvent& event)
{
	// closing while it runs cancels (the thread finishes its current step first)
	if (m_threadState == ThreadRunning)
		m_threadState = ThreadCanceled;

	m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();

	event.Skip();
}

void DownloadGraphicPacksWindow::OnUpdate(const wxTimerEvent&)
{
	if (m_threadState != ThreadRunning)
	{
		Close();
		return;
	}

	const DownloadStage_t stage = m_stage;
	if (stage != m_currentStage)
	{
		if (stage == StageDownloading)
			SetTitle(_("Downloading graphic packs..."));
		else if (stage == StageExtracting)
			SetTitle(_("Extracting..."));
		m_currentStage = stage;
	}

	double fraction;
	if (m_currentStage == StageDownloading)
		fraction = m_downloadState->progress;
	else if (m_currentStage == StageExtracting)
		fraction = m_extractionProgress;
	else
		return;
	const sint32 percent = (sint32)(fraction * 100.0f);
	if (m_processBar->GetValue() != percent)
		m_processBar->SetValue(percent);
}

void DownloadGraphicPacksWindow::OnCancelButton(const wxCommandEvent&)
{
	Close();
}
