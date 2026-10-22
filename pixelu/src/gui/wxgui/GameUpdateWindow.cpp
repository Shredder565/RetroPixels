// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "wxgui/GameUpdateWindow.h"
#include "util/helpers/helpers.h"
#include "util/helpers/SystemException.h"
#include "Cafe/TitleList/GameInfo.h"
#include "config/ActiveSettings.h"
#include "wxgui/helpers/wxHelpers.h"

namespace
{
	wxString TitleKindName(TitleId titleId)
	{
		switch (TitleIdParser(titleId).GetType())
		{
		case TitleIdParser::TITLE_TYPE::AOC: return _("DLC");
		case TitleIdParser::TITLE_TYPE::BASE_TITLE: return _("Base game");
		case TitleIdParser::TITLE_TYPE::BASE_TITLE_DEMO: return _("Demo");
		case TitleIdParser::TITLE_TYPE::SYSTEM_TITLE:
		case TitleIdParser::TITLE_TYPE::SYSTEM_OVERLAY_TITLE: return _("System title");
		case TitleIdParser::TITLE_TYPE::SYSTEM_DATA: return _("System data title");
		case TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE: return _("Update");
		default: return "Unknown";
		}
	}

	bool AskYesNo(wxWindow* parent, const wxString& text, long extraStyle = 0)
	{
		wxMessageDialog ask(parent, text, _("Warning"), wxCENTRE | wxYES_NO | extraStyle);
		return ask.ShowModal() == wxID_YES;
	}
}

GameUpdateWindow::GameUpdateWindow(wxWindow& parent, const fs::path& filePath)
	: wxDialog(&parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX),
	  m_thread_state(ThreadRunning)
{
	try
	{
#if BOOST_OS_WINDOWS
		SetLastError(0);
#endif
		if (!ParseUpdate(filePath))
			throw AbortException();
	}
	catch (const std::runtime_error& ex)
	{
		throw SystemException(ex);
	}

	const TitleIdParser kind(GetTitleId());
	if (kind.GetType() == TitleIdParser::TITLE_TYPE::AOC)
		SetTitle(_("Installing DLC..."));
	else if (kind.GetType() == TitleIdParser::TITLE_TYPE::BASE_TITLE_UPDATE)
		SetTitle(_("Installing update..."));
	else if (kind.IsSystemTitle())
		SetTitle(_("Installing system title..."));
	else
		SetTitle(_("Installing title..."));

	auto* layout = new wxBoxSizer(wxVERTICAL);

	// the bar counts kilobytes
	m_processBar = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(500, 20), wxGA_HORIZONTAL);
	m_processBar->SetValue(0);
	m_processBar->SetRange((sint32)(m_required_size / 1000));
	layout->Add(m_processBar, 0, wxALL | wxEXPAND, 5);

	auto* cancel = new wxButton(this, wxID_ANY, _("Cancel"));
	cancel->Bind(wxEVT_BUTTON, &GameUpdateWindow::OnCancelButton, this);
	layout->Add(cancel, 0, wxALIGN_RIGHT | wxALL, 5);

	SetSizer(layout);
	Centre(wxBOTH);
	Layout();
	Fit();

	m_timer = new wxTimer(this);
	Bind(wxEVT_TIMER, &GameUpdateWindow::OnUpdate, this);
	Bind(wxEVT_CLOSE_WINDOW, &GameUpdateWindow::OnClose, this);
	m_timer->Start(250);

	m_thread_state = ThreadRunning;
	m_thread = std::thread(&GameUpdateWindow::ThreadWork, this);
}

GameUpdateWindow::~GameUpdateWindow()
{
	m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();
}

bool GameUpdateWindow::ParseUpdate(const fs::path& metaPath)
{
	m_title_info = TitleInfo(metaPath);
	if (!m_title_info.IsValid())
		return false;

	const fs::path target = ActiveSettings::GetMlcPath(m_title_info.GetInstallPath());
	std::error_code ec;
	if (fs::exists(target, ec))
	{
		try
		{
			// something is installed there already: make sure the user wants it replaced
			const TitleInfo installed(target);
			if (installed.IsValid())
			{
				const uint32 newVersion = m_title_info.GetAppTitleVersion();
				const uint32 oldVersion = installed.GetAppTitleVersion();
				if (TitleIdParser(m_title_info.GetAppTitleId()).GetType() != TitleIdParser(installed.GetAppTitleId()).GetType())
				{
					const wxString text = formatWxString(_("It seems that there is already a title installed at the target location but it has a different type.\nCurrently installed: \'{}\' Installing: \'{}\'\n\nThis can happen for titles which were installed with very old Cemu versions.\nDo you still want to continue with the installation? It will replace the currently installed title."),
														 TitleKindName(installed.GetAppTitleId()), TitleKindName(m_title_info.GetAppTitleId()));
					if (!AskYesNo(this, text, wxICON_EXCLAMATION))
						return false;
				}
				else if (oldVersion == newVersion)
				{
					if (!AskYesNo(this, _("It seems that the selected title is already installed, do you want to reinstall it?")))
						return false;
				}
				else if (oldVersion > newVersion)
				{
					if (!AskYesNo(this, _("It seems that a newer version is already installed, do you still want to install the older version?")))
						return false;
				}
			}
			// (an invalid installation is simply replaced)

			// move it aside as <folder>.backup, clearing out an old backup first
			m_backup_folder = target;
			m_backup_folder.replace_extension(".backup");
			std::error_code backupError;
			while (fs::exists(m_backup_folder, backupError) || backupError)
			{
				fs::remove_all(m_backup_folder, backupError);
				if (backupError)
				{
					wxMessageBox(formatWxString(_("Error when trying to move former title installation:\n{}"), GetSystemErrorMessage(backupError)), _("Error"), wxOK | wxCENTRE, this);
					return false;
				}
				// give the file system a moment before looking again
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			fs::rename(target, m_backup_folder);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "GameUpdateWindow::ParseUpdate exist-error: {} at {}", ex.what(), _pathToUtf8(target));
		}
	}
	m_target_path = target;

	m_source_paths = {metaPath / "content", metaPath / "code", metaPath / "meta"};

	m_required_size = 0;
	for (const fs::path& folder : m_source_paths)
	{
		for (const fs::directory_entry& item : fs::recursive_directory_iterator(folder))
		{
			if (is_regular_file(item.path()))
				m_required_size += file_size(item.path());
		}
	}

	const fs::space_info space = fs::space(ActiveSettings::GetMlcPath());
	if (space.free <= m_required_size)
	{
		const wxString text = formatWxString(_("Not enough space available.\nRequired: {0} MB\nAvailable: {1} MB"), m_required_size / 1024 / 1024, space.free / 1024 / 1024);
		throw std::runtime_error(text.utf8_string());
	}
	return true;
}

void GameUpdateWindow::ThreadWork()
{
	fs::directory_entry current;
	try
	{
		// target/content, target/code, target/meta
		for (const fs::path& folder : m_source_paths)
		{
			if (folder.has_stem())
				create_directories(m_target_path / folder.stem());
		}

		for (const fs::path& folder : m_source_paths)
		{
			if (m_thread_state == ThreadCanceled)
				break;
			if (!folder.has_parent_path())
				continue;

			// paths below the title folder, e.g. "content/0000.app"
			const size_t prefixLength = folder.parent_path().string().size() + 1;
			for (const fs::directory_entry& item : fs::recursive_directory_iterator{folder})
			{
				if (m_thread_state == ThreadCanceled)
					break;
				current = item;
				const fs::path destination = m_target_path / fs::path(item.path().string().substr(prefixLength));
				if (is_directory(item))
				{
					create_directories(destination);
					continue;
				}
				copy(item, destination, fs::copy_options::overwrite_existing);
				if (is_regular_file(item.path()))
					m_processed_size += file_size(item.path());
			}
		}
	}
	catch (const std::exception& ex)
	{
		std::string message = GetSystemErrorMessage(ex);
		if (current != fs::directory_entry{})
			message += fmt::format("\n{}\n{}", _("Current file:").utf8_string(), _pathToUtf8(current.path()));
		m_thread_exception = std::move(message);
		m_thread_state = ThreadCanceled;
	}

	if (m_thread_state != ThreadCanceled)
	{
		m_thread_state = ThreadFinished;
		return;
	}
	// cancelled or failed: nothing half-copied stays behind
	std::error_code ec;
	if (fs::exists(m_target_path, ec))
		fs::remove_all(m_target_path, ec);
}

int GameUpdateWindow::ShowModal()
{
	wxDialog::ShowModal();
	return m_thread_state == ThreadCanceled ? wxID_CANCEL : wxID_OK;
}

void GameUpdateWindow::OnClose(wxCloseEvent& event)
{
	if (m_thread_state == ThreadRunning)
	{
		wxMessageDialog ask(this, _("Do you really want to cancel the installation process?\n\nCanceling the process will delete the applied files."), _("Info"), wxCENTRE | wxYES_NO);
		if (ask.ShowModal() != wxID_YES)
			return;
		m_thread_state = ThreadCanceled;
	}

	m_timer->Stop();
	if (m_thread.joinable())
		m_thread.join();

	if (!m_backup_folder.empty())
	{
		try
		{
			if (m_thread_state == ThreadCanceled)
			{
				// put the previous installation back
				if (fs::exists(m_target_path))
					fs::remove_all(m_target_path);
				fs::rename(m_backup_folder, m_target_path);
			}
			else if (fs::exists(m_backup_folder))
				fs::remove_all(m_backup_folder);
		}
		catch (const std::exception& ex)
		{
			if (m_thread_state == ThreadCanceled)
				cemuLog_logDebug(LogType::Force, "can't restore update backup: {}", ex.what());
			else
				cemuLog_logDebug(LogType::Force, "can't delete update backup: {}", ex.what());
		}
		m_backup_folder.clear();
	}

	event.Skip();
}

void GameUpdateWindow::OnUpdate(const wxTimerEvent&)
{
	if (m_thread_state != ThreadRunning)
	{
		Close();
		return;
	}
	const sint32 kilobytes = (sint32)(m_processed_size / 1000);
	if (m_processBar->GetValue() != kilobytes)
		m_processBar->SetValue(kilobytes);
}

void GameUpdateWindow::OnCancelButton(const wxCommandEvent&)
{
	Close();
}
