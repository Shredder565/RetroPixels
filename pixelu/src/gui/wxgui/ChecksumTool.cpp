// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/ChecksumTool.h"

#include "Cafe/TitleList/GameInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/Filesystem/WUD/wud.h"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"
#include "wxgui/helpers/wxCustomEvents.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/wxHelper.h"

#include <zip.h>
#include <curl/curl.h>

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/schema.h>

#include <wx/button.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/frame.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

namespace
{
	// what a checksum file must look like
	constexpr char kChecksumSchema[] = R"json({
	"$schema": "http://json-schema.org/draft-04/schema#",
	"type": "object",
	"required": ["title_id", "region", "version", "files"],
	"properties": {
		"title_id": {"type": "string"},
		"region": {"type": "integer"},
		"version": {"type": "integer"},
		"wud_hash": {"type": "string"},
		"files": {
			"type": "array",
			"items": {
				"type": "object",
				"required": ["file", "hash"],
				"properties": {
					"file": {"type": "string"},
					"hash": {"type": "string"}
				}
			}
		}
	}
})json";

	constexpr const char* kRepoCommitUrl = "https://api.github.com/repos/cemu-project/title-checksums/commits/master";
	constexpr const char* kRepoArchiveUrl = "https://github.com/cemu-project/title-checksums/archive/master.zip";
	constexpr std::string_view kArchiveRoot = "title-checksums-master/";
	constexpr uint64 kMaxArchiveFileSize = 128ull * 1024 * 1024;

	size_t AppendToString(const char* data, size_t size, size_t count, std::string* out)
	{
		out->append(data, size * count);
		return size * count;
	}

	// GET url into body; returns the HTTP status (0 when the request failed)
	long HttpGet(const char* url, bool followRedirects, std::string& body)
	{
		CURL* curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendToString);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1);
		curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
		if (followRedirects)
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, BUILD_VERSION_WITH_NAME_STRING);
		curl_easy_perform(curl);
		long status = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_cleanup(curl);
		return status;
	}

	std::string HexUpper(const uint8* bytes, size_t count)
	{
		std::string text;
		text.reserve(count * 2);
		for (size_t i = 0; i < count; ++i)
			text += fmt::format("{:02X}", bytes[i]);
		return text;
	}

	// writes every file of the downloaded repository archive below folder; false if it isn't a zip
	bool UnpackChecksumArchive(const std::string& archive, const fs::path& folder)
	{
		zip_error_t error;
		zip_error_init(&error);
		zip_source_t* source = zip_source_buffer_create(archive.data(), archive.size(), 0, &error);
		if (!source)
		{
			zip_error_fini(&error);
			return false;
		}
		zip_t* zip = zip_open_from_source(source, ZIP_RDONLY, &error);
		if (!zip)
		{
			zip_source_free(source);
			zip_error_fini(&error);
			return false;
		}

		const sint64 count = zip_get_num_entries(zip, 0);
		for (sint64 i = 0; i < count; ++i)
		{
			zip_stat_t info{};
			if (zip_stat_index(zip, i, 0, &info) != 0)
				continue;
			const std::string_view name = info.name;
			if (name.find("../") != std::string_view::npos || name.find("..\\") != std::string_view::npos)
				continue; // never write outside the folder
			if (name.size() <= kArchiveRoot.size() || name.substr(0, kArchiveRoot.size()) != kArchiveRoot)
				continue; // the root folder itself, or not part of it
			const fs::path target = folder / std::string(name.substr(kArchiveRoot.size()));

			if (name.back() == '/')
			{
				std::error_code ec;
				fs::create_directories(target, ec);
				continue;
			}
			if (info.size == 0 || info.size > kMaxArchiveFileSize)
				continue;

			zip_file_t* entry = zip_fopen_index(zip, i, 0);
			if (!entry)
				continue;
			std::vector<char> data(info.size);
			if (zip_fread(entry, data.data(), info.size) == (zip_int64_t)info.size)
			{
				std::ofstream out(target);
				if (out.is_open())
					out.write(data.data(), (std::streamsize)data.size());
			}
			zip_fclose(entry);
		}
		zip_discard(zip);
		zip_error_fini(&error);
		return true;
	}

	// every file below basePath + relative, as paths relative to basePath with '/' separators
	void CollectFiles(std::set<std::string>& out, const std::string& basePath, const std::string& relative)
	{
		sint32 status;
		FSCVirtualFile* dir = fsc_openDirIterator((basePath + relative).c_str(), &status);
		cemu_assert(dir);
		FSCDirEntry entry;
		while (fsc_nextDir(dir, &entry))
		{
			std::string path = std::string(relative).append(entry.GetPath());
			if (entry.isDirectory)
				CollectFiles(out, basePath, path + "/");
			else
				out.emplace(std::move(path));
		}
		delete dir;
	}

	std::string ChecksumFileName(uint64 titleId, uint32 version)
	{
		return fmt::format("{:016x}_v{}.json", titleId, version);
	}

	bool ParseJsonFile(const fs::path& file, rapidjson::Document& doc, wxWindow* parent)
	{
		std::ifstream in(file);
		if (!in.is_open())
		{
			wxMessageBox(_("Can't open file!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, parent);
			return false;
		}
		rapidjson::IStreamWrapper stream(in);
		doc.ParseStream(stream);
		if (doc.HasParseError())
		{
			wxMessageBox(_("Can't parse JSON file!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, parent);
			return false;
		}
		return true;
	}
}

ChecksumTool::ChecksumTool(wxWindow* parent, wxTitleManagerList::TitleEntry& entry)
	: wxDialog(parent, wxID_ANY,
			   formatWxString(_("Title checksum of {:08x}-{:08x}"), (uint32)(entry.title_id >> 32), (uint32)(entry.title_id & 0xFFFFFFFF)),
			   wxDefaultPosition, wxDefaultSize, wxCAPTION | wxFRAME_TOOL_WINDOW | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_entry(entry)
{
	m_info = CafeTitleList::GetTitleInfoByUID(m_entry.location_uid);
	if (!m_info.IsValid())
		throw std::runtime_error("Invalid title");

	// the online data is refreshed by the first checksum window of the session only
	static bool s_refreshStarted = false;
	if (s_refreshStarted)
		m_enable_verify_button = 1;
	else
	{
		s_refreshStarted = true;
		m_online_ready = std::async(std::launch::async, &ChecksumTool::LoadOnlineData, this);
	}

	auto* layout = new wxBoxSizer(wxVERTICAL);

	{
		auto* group = new wxStaticBoxSizer(new wxStaticBox(this, wxID_ANY, _("Verifying integrity of game files...")), wxVERTICAL);
		wxWindow* box = group->GetStaticBox();

		m_progress = new wxGauge(box, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize, wxGA_HORIZONTAL | wxGA_SMOOTH);
		m_progress->SetMinSize({400, -1});
		m_progress->SetValue(0);
		group->Add(m_progress, 0, wxALL | wxEXPAND, 5);

		m_status = new wxStaticText(box, wxID_ANY, wxEmptyString);
		m_status->Wrap(-1);
		group->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 5);

		layout->Add(group, 0, wxEXPAND | wxALL, 5);
	}

	{
		auto* group = new wxStaticBoxSizer(new wxStaticBox(this, wxID_ANY, _("Control")), wxHORIZONTAL);
		wxWindow* box = group->GetStaticBox();

		auto addButton = [&](const wxString& label, const wxString& tip, void (ChecksumTool::*handler)(wxCommandEvent&)) {
			auto* button = new wxButton(box, wxID_ANY, label);
			button->SetToolTip(tip);
			button->Disable();
			button->Bind(wxEVT_BUTTON, handler, this);
			group->Add(button, 0, wxALL | wxEXPAND, 5);
			return button;
		};
		m_verify_online = addButton(_("Verify online"), _("Verifies the checksum online"), &ChecksumTool::OnVerifyOnline);
		m_verify_local = addButton(_("Verify with local file"), _("Verifies the checksum with a local JSON file you can select"), &ChecksumTool::OnVerifyLocal);
		m_export_button = addButton(_("Export"), _("Export the title checksum data to a local JSON file"), &ChecksumTool::OnExportChecksums);

		// sent by the hashing and by the download; the second one enables the button when the
		// repository has a file for this title and version
		m_verify_online->Bind(wxEVT_ENABLE, [this](wxCommandEvent&) {
			if (++m_enable_verify_button < 2)
				return;
			const fs::path file = ActiveSettings::GetUserDataPath("resources/checksums/{}", ChecksumFileName(m_json_entry.title_id, m_info.GetAppTitleVersion()));
			if (exists(file))
				m_verify_online->Enable();
		});

		layout->Add(group, 0, wxEXPAND | wxALL, 5);
	}

	Bind(wxEVT_SET_GAUGE_VALUE, &ChecksumTool::OnSetGaugevalue, this);
	m_worker = std::thread(&ChecksumTool::DoWork, this);

	SetSizerAndFit(layout);
	Centre(wxBOTH);
}

ChecksumTool::~ChecksumTool()
{
	m_running = false;
	if (m_worker.joinable())
		m_worker.join();
}

void ChecksumTool::LoadOnlineData() const
{
	try
	{
		const fs::path folder = ActiveSettings::GetUserDataPath("resources/checksums");
		std::string newestCommit;
		bool download = true;

		if (exists(folder))
		{
			// skip the download when commit.txt names the repository's newest commit
			std::string localCommit;
			{
				std::ifstream in(folder / "commit.txt");
				if (in.is_open())
					std::getline(in, localCommit);
			}
			std::string reply;
			if (HttpGet(kRepoCommitUrl, false, reply) == 200 && !reply.empty())
			{
				rapidjson::Document doc;
				doc.Parse(reply.c_str(), reply.size());
				if (!doc.HasParseError() && doc.HasMember("sha"))
				{
					newestCommit = doc["sha"].GetString();
					download = !boost::iequals(localCommit, newestCommit);
				}
			}
		}
		else
			fs::create_directories(folder);

		if (download)
		{
			std::string archive;
			if (HttpGet(kRepoArchiveUrl, true, archive) == 200 && !archive.empty() && UnpackChecksumArchive(archive, folder))
			{
				std::ofstream out(folder / "commit.txt");
				if (out.is_open())
					out << newestCommit;
			}
		}
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "error on updating json checksum data: {}", ex.what());
	}

	wxQueueEvent(m_verify_online, new wxCommandEvent(wxEVT_ENABLE));
}

void ChecksumTool::OnSetGaugevalue(wxSetGaugeValue& event)
{
	event.GetGauge()->SetValue(event.GetValue());
	event.GetTextCtrl()->SetLabelText(event.GetText());

	// hashing finished
	if (event.GetInt() == 0 && event.GetValue() == 100)
	{
		m_export_button->Enable();
		m_verify_local->Enable();
		wxPostEvent(m_verify_online, wxCommandEvent(wxEVT_ENABLE));
	}
}

void ChecksumTool::OnExportChecksums(wxCommandEvent&)
{
	wxDirDialog picker(this, _("Export checksum entry"), "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
	if (picker.ShowModal() != wxID_OK || picker.GetPath().IsEmpty())
		return;

	const std::string titleIdText = fmt::format("{:016x}", m_json_entry.title_id);

	rapidjson::Document doc;
	doc.SetObject();
	auto& alloc = doc.GetAllocator();
	doc.AddMember("title_id", rapidjson::StringRef(titleIdText.c_str(), titleIdText.size()), alloc);
	doc.AddMember("region", (int)m_info.GetMetaRegion(), alloc);
	doc.AddMember("version", m_info.GetAppTitleVersion(), alloc);
	if (!m_json_entry.wud_hash.empty())
		doc.AddMember("wud_hash", rapidjson::StringRef(m_json_entry.wud_hash.c_str(), m_json_entry.wud_hash.size()), alloc);

	rapidjson::Value files(rapidjson::kArrayType);
	for (const auto& [name, hash] : m_json_entry.file_hashes)
	{
		rapidjson::Value item(rapidjson::kObjectType);
		item.AddMember("file", rapidjson::StringRef(name.c_str(), name.size()), alloc);
		item.AddMember("hash", rapidjson::StringRef(hash.c_str(), hash.size()), alloc);
		files.PushBack(item, alloc);
	}
	doc.AddMember("files", files, alloc);

	fs::path target{picker.GetPath().c_str().AsInternal()};
	target /= ChecksumFileName(m_json_entry.title_id, m_info.GetAppTitleVersion());

	std::ofstream out(target);
	if (!out.is_open())
	{
		wxMessageBox(formatWxString(_("Can't write to file: {}"), target.string()), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}
	rapidjson::OStreamWrapper stream(out);
	rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(stream);
	doc.Accept(writer);
	wxMessageBox(_("Export successful"), wxMessageBoxCaptionStr, wxOK | wxCENTRE, this);
}

void ChecksumTool::VerifyJsonEntry(const rapidjson::Document& doc)
{
	auto showError = [this](const wxString& text, long icon = wxICON_ERROR) {
		wxMessageBox(text, _("Error"), wxOK | wxCENTRE | icon, this);
	};

	{
		rapidjson::Document schemaJson;
		schemaJson.Parse(kChecksumSchema);
		wxASSERT(!schemaJson.HasParseError());
		rapidjson::SchemaDocument schema(schemaJson);
		rapidjson::SchemaValidator validator(schema);
		if (!doc.Accept(validator))
		{
			showError(_("JSON file doesn't satisfy needed schema"));
			return;
		}
	}

	try
	{
		JsonEntry expected{};
		expected.title_id = ConvertString<uint64>(doc["title_id"].GetString(), 16);
		expected.region = (CafeConsoleRegion)doc["region"].GetInt();
		expected.version = doc["version"].GetInt();
		if (doc.HasMember("wud_hash"))
			expected.wud_hash = doc["wud_hash"].GetString();
		for (const auto& item : doc["files"].GetArray())
		{
			// stored with forward slashes, like our own list
			const fs::path name(item["file"].GetString(), fs::path::generic_format);
			expected.file_hashes[name.generic_string()] = item["hash"].GetString();
		}

		if (expected.title_id != m_json_entry.title_id)
		{
			showError(_("The file you are comparing with is for a different title."));
			return;
		}
		if (expected.version != m_json_entry.version)
		{
			showError(formatWxString(_("Wrong version: {}"), expected.version));
			return;
		}
		if (expected.region != m_json_entry.region)
		{
			showError(formatWxString(_("Wrong region: {}"), expected.region));
			return;
		}

		if (!m_json_entry.wud_hash.empty())
		{
			// whole image
			if (expected.wud_hash.empty())
			{
				showError(_("The verification data doesn't include a WUD hash!"), wxICON_WARNING);
				return;
			}
			if (!boost::iequals(expected.wud_hash, m_json_entry.wud_hash))
			{
				showError(formatWxString(_("Your game image is invalid!\n\nYour hash:\n{}\n\nExpected hash:\n{}"), m_json_entry.wud_hash, expected.wud_hash));
				return;
			}
		}
		else
		{
			// file by file
			std::vector<std::string_view> missing;
			std::map<std::string_view, std::pair<std::string, std::string>> damaged; // name -> (ours, expected)
			for (const auto& [name, hash] : expected.file_hashes)
			{
				const auto ours = m_json_entry.file_hashes.find(name);
				if (ours == m_json_entry.file_hashes.cend())
					missing.emplace_back(name);
				else if (!boost::iequals(hash, ours->second))
					damaged[name] = {ours->second, hash};
			}

			const size_t problems = missing.size() + damaged.size();
			if (problems > 0 && problems < 30)
			{
				// short enough to show in the message box
				std::string text;
				if (!missing.empty())
				{
					text += _("The following files are missing:").utf8_string() + "\n";
					for (const auto& name : missing)
						text.append(name).append("\n");
					if (!damaged.empty())
						text += "\n";
				}
				if (!damaged.empty())
				{
					text += _("The following files are damaged:").utf8_string() + "\n";
					for (const auto& entry : damaged)
						text.append(entry.first).append("\n");
				}
				showError(wxString::FromUTF8(text));
				return;
			}
			if (problems > 0)
			{
				const wxString question = missing.empty()
					? formatWxString(_("{} files have an invalid hash!\nDo you want to export a list of them to a file?"), damaged.size())
					: _("Multiple issues with your game files have been found!\nDo you want to export them to a file?");
				if (wxMessageBox(question, _("Error"), wxYES_NO | wxCENTRE | wxICON_ERROR, this) != wxYES)
					return;

				wxFileDialog picker(this, _("Select a file to export the errors"), wxEmptyString, wxEmptyString, "Error list (*.txt)|*.txt", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
				if (picker.ShowModal() != wxID_OK || picker.GetPath().IsEmpty())
					return;
				const std::string reportPath = picker.GetPath().utf8_string();
				std::ofstream report(_utf8ToPath(reportPath));
				if (!report.is_open())
				{
					showError(_("Can't open file to write!"));
					return;
				}
				if (!missing.empty())
				{
					report << "The following files are missing:\n";
					for (const auto& name : missing)
						report << "\t" << name << "\n";
					report << "\n";
				}
				if (!damaged.empty())
				{
					report << "The following files have an invalid hash (name | current hash | expected hash):\n";
					for (const auto& [name, hashes] : damaged)
						report << "\t" << name << " | " << hashes.first << " | " << hashes.second << "\n";
				}
				report.close();
				wxLaunchDefaultBrowser(formatWxString("file:{}", reportPath));
				return;
			}
		}
		wxMessageBox(_("Your game files are valid"), _("Success"), wxOK | wxCENTRE, this);
	}
	catch (const std::exception& ex)
	{
		showError(formatWxString(_("JSON parse error: {}"), ex.what()));
	}
}

void ChecksumTool::OnVerifyOnline(wxCommandEvent&)
{
	const fs::path file = ActiveSettings::GetUserDataPath("resources/checksums/{}", ChecksumFileName(m_json_entry.title_id, m_info.GetAppTitleVersion()));
	if (!exists(file))
		return;
	rapidjson::Document doc;
	if (ParseJsonFile(file, doc, this))
		VerifyJsonEntry(doc);
}

void ChecksumTool::OnVerifyLocal(wxCommandEvent&)
{
	const std::string suggested = ChecksumFileName(m_json_entry.title_id, m_info.GetAppTitleVersion());
	wxFileDialog picker(this, _("Open checksum entry"), "", suggested.c_str(), "JSON files (*.json)|*.json", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (picker.ShowModal() != wxID_OK || picker.GetPath().IsEmpty())
		return;
	rapidjson::Document doc;
	if (ParseJsonFile(fs::path{picker.GetPath().c_str().AsInternal()}, doc, this))
		VerifyJsonEntry(doc);
}

void ChecksumTool::DoWork()
{
	m_json_entry.title_id = m_info.GetAppTitleId();
	m_json_entry.region = m_info.GetMetaRegion();
	m_json_entry.version = m_info.GetAppTitleVersion();

	auto progress = [this](int percent, const wxString& text) {
		wxQueueEvent(this, new wxSetGaugeValue(percent, m_progress, m_status, text));
	};
	auto stopped = [this] { return !m_running.load(std::memory_order_relaxed); };

	static_assert(SHA256_DIGEST_LENGTH == 32);
	std::array<uint8, SHA256_DIGEST_LENGTH> digest{};

	if (m_info.GetFormat() == TitleInfo::TitleDataFormat::WUD)
	{
		// disc images are hashed as a whole; reading is 90% of the bar
		const std::string imageName = m_entry.path.string();
		progress(1, formatWxString(_("Reading game image: {}"), imageName));

		wud_t* image = wud_open(m_info.GetPath());
		if (!image)
		{
			progress(0, "can't open game image");
			return;
		}
		const uint64 total = wud_getWUDSize(image);
		std::vector<uint8> chunk(8 * 1024 * 1024);
		EVP_MD_CTX* sha = EVP_MD_CTX_new();
		EVP_DigestInit(sha, EVP_sha256());

		uint64 done = 0;
		for (;;)
		{
			if (stopped())
			{
				wud_close(image);
				EVP_MD_CTX_free(sha);
				return;
			}
			const size_t want = (size_t)std::min<uint64>(chunk.size(), total - done);
			const uint32 got = wud_readData(image, chunk.data(), want, done);
			done += got;
			EVP_DigestUpdate(sha, chunk.data(), got);
			progress((int)((done * 90) / total), formatWxString(_("Reading game image: {0}/{1} kB"), done / 1024, total / 1024));
			if (got == 0 || done >= total)
				break;
		}
		wud_close(image);

		progress(90, formatWxString(_("Generating checksum of game image: {}"), imageName));
		if (stopped())
		{
			EVP_MD_CTX_free(sha);
			return;
		}
		EVP_DigestFinal_ex(sha, digest.data(), nullptr);
		EVP_MD_CTX_free(sha);
		m_json_entry.wud_hash = HexUpper(digest.data(), digest.size());

		progress(100, formatWxString(_("Generated checksum of game image: {}"), imageName));
		return;
	}

	// every other format: mount the title and hash each file
	const std::string mountPath = TitleInfo::GetUniqueTempMountingPath();
	m_info.Mount(mountPath.c_str(), "", FSC_PRIORITY_BASE);
	progress(1, _("Grabbing game files"));

	std::set<std::string> files;
	CollectFiles(files, mountPath, "");

	const size_t fileCount = files.size();
	size_t hashed = 0;
	for (const std::string& name : files)
	{
		auto data = fsc_extractFile((mountPath + "/" + name).c_str());
		if (!data)
		{
			cemuLog_log(LogType::Force, "Failed to open {}", name);
			continue;
		}
		SHA256(data->data(), data->size(), digest.data());
		m_json_entry.file_hashes[name] = HexUpper(digest.data(), digest.size());

		++hashed;
		progress((int)((hashed * 100) / fileCount), formatWxString(_("Hashing game file: {}/{}"), hashed, fileCount));
		if (stopped())
		{
			m_info.Unmount(mountPath.c_str());
			return;
		}
	}
	m_info.Unmount(mountPath.c_str());
	progress(100, formatWxString(_("Generated checksum of {} game files"), fileCount));
}
