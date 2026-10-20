// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/TitleManager.h"

#include "wxgui/helpers/wxCustomEvents.h"
#include "wxgui/helpers/wxCustomData.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/wxHelper.h"
#include "wxgui/components/wxTitleManagerList.h"
#include "wxgui/components/wxDownloadManagerList.h"
#include "wxgui/GameUpdateWindow.h"
#include "wxgui/dialogs/SaveImport/SaveTransfer.h"
#include "wxgui/dialogs/SaveImport/SaveImportWindow.h"
#include "wxgui/CemuApp.h"

#include "Cafe/TitleList/GameInfo.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cafe/Account/Account.h"
#include "Cemu/Tools/DownloadManager/DownloadManager.h"
#include "config/ActiveSettings.h"
#include "util/helpers/helpers.h"
#include "resource/embedded/resources.h"

#include <wx/listbase.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>
#include <wx/textctrl.h>
#include <wx/panel.h>
#include <wx/bmpbuttn.h>
#include <wx/choice.h>
#include <wx/checkbox.h>
#include <wx/msgdlg.h>
#include <wx/filedlg.h>
#include <wx/notebook.h>
#include <wx/settings.h>

#include <pugixml.hpp>
#include <zip.h>

#include <cctype>
#include <cstdlib>

// This window's own "title found" event. The title list announces new titles to itself with an
// event of the same name that is local to wxTitleManagerList.cpp, so this one is never sent today.
wxDEFINE_EVENT(wxEVT_TITLE_FOUND, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_TITLE_SEARCH_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_DL_TITLE_UPDATE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_DL_DISCONNECT_COMPLETE, wxCommandEvent);

namespace
{
	using DlEntry = wxDownloadManagerList::TitleEntry;
	using DlKind = wxDownloadManagerList::EntryType;
	using DlStatus = wxDownloadManagerList::TitleDownloadStatus;
	using ReportStatus = DlMgrTitleReport::STATUS;

	// ---- pure helpers (TM_PURE_BEGIN)

	// a save's per-account folder is named by the 8 hex digit persistent id
	bool IsPersistentIdFolderName(const std::string& name)
	{
		if (name.size() != 8)
			return false;
		for (const char c : name)
		{
			if (!std::isxdigit((unsigned char)c))
				return false;
		}
		return true;
	}

	// the upper word's low nibble tells the title kind: E = update, C = DLC, anything else = game
	DlKind DownloadKindOf(uint64 titleId)
	{
		switch ((titleId >> 32) & 0xF)
		{
		case 0xE: return DlKind::Update;
		case 0xC: return DlKind::DLC;
		default: return DlKind::Base;
		}
	}

	bool IsInstallableReport(ReportStatus status)
	{
		return status == ReportStatus::INSTALLABLE || status == ReportStatus::INSTALLABLE_UNFINISHED || status == ReportStatus::INSTALLABLE_UPDATE;
	}

	// for installable titles the list draws "progress" as a marker: 1 = resume unfinished download, 2 = update
	uint32 InstallableMarker(ReportStatus status)
	{
		if (status == ReportStatus::INSTALLABLE_UNFINISHED)
			return 1;
		if (status == ReportStatus::INSTALLABLE_UPDATE)
			return 2;
		return 0;
	}

	DlStatus PackageStatusOf(ReportStatus status)
	{
		switch (status)
		{
		case ReportStatus::INITIALIZING: return DlStatus::Initializing;
		case ReportStatus::QUEUED: return DlStatus::Queued;
		case ReportStatus::CHECKING: return DlStatus::Checking;
		case ReportStatus::DOWNLOADING: return DlStatus::Downloading;
		case ReportStatus::VERIFYING: return DlStatus::Verifying;
		case ReportStatus::INSTALLING: return DlStatus::Installing;
		case ReportStatus::INSTALLED: return DlStatus::Installed;
		case ReportStatus::HAS_ERROR: return DlStatus::Error;
		default: return DlStatus::None; // includes PAUSED
		}
	}

	// the save account picker's text: hex id, plus the Mii name when the account exists here
	std::string SaveAccountLabel(uint32 persistentId, const std::wstring_view* miiName)
	{
		if (!miiName)
			return fmt::format("{:x}", persistentId);
		return fmt::format("{:x} ({})", persistentId, boost::nowide::narrow(miiName->data(), miiName->size()));
	}

	// ---- TM_PURE_END

	DlEntry MakeDownloadEntry(const DlMgrTitleReport& report)
	{
		const bool installable = IsInstallableReport(report.status);
		DlEntry entry(DownloadKindOf(report.titleId), !installable, report.titleId, report.version, report.isPaused);
		entry.name = wxString::FromUTF8(report.name);
		if (installable)
		{
			entry.status = DlStatus::Available;
			entry.progress = InstallableMarker(report.status);
		}
		else
		{
			entry.status = PackageStatusOf(report.status);
			entry.progress = report.progress;
			entry.progressMax = report.progressMax;
			if (entry.status == DlStatus::Error)
				entry.errorMsg = report.errorMsg;
		}
		return entry;
	}

	std::string AccountLabel(uint32 persistentId)
	{
		for (const Account& account : Account::GetAccounts())
		{
			if (account.GetPersistentId() == persistentId)
			{
				const std::wstring_view mii = account.GetMiiName();
				return SaveAccountLabel(persistentId, &mii);
			}
		}
		return SaveAccountLabel(persistentId, nullptr);
	}

	fs::path SaveAccountFolder(uint64 titleId, uint32 persistentId)
	{
		return ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/user/{:08x}", (uint32)(titleId >> 32), (uint32)(titleId & 0xFFFFFFFF), persistentId);
	}

	fs::path SaveInfoFile(uint64 titleId)
	{
		return ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/meta/saveinfo.xml", (uint32)(titleId >> 32), (uint32)(titleId & 0xFFFFFFFF));
	}

	bool IsExistingFolder(const fs::path& path)
	{
		std::error_code ec;
		return fs::is_directory(path, ec);
	}

	// fills the account ids of a save entry from its non-empty user/<id> folders (once)
	void LoadSavePersistentIds(wxTitleManagerList::TitleEntry& entry)
	{
		if (!entry.persistent_ids.empty())
			return;
		cemu_assert(entry.type == wxTitleManagerList::EntryType::Save);
		const SaveInfo save = CafeSaveList::GetSaveByTitleId(entry.title_id);
		if (!save.IsValid())
			return;
		std::error_code ec;
		for (const auto& child : fs::directory_iterator(save.GetPath() / "user", ec))
		{
			if (!child.is_directory(ec) || fs::is_empty(child.path()))
				continue;
			const std::string name = child.path().filename().string();
			if (IsPersistentIdFolderName(name))
				entry.persistent_ids.emplace_back((uint32)std::strtoul(name.c_str(), nullptr, 16));
		}
	}

	// removes the account's <info> child from the save's saveinfo.xml; true when the file was rewritten
	bool RemoveFromSaveInfo(const fs::path& saveInfo, uint32 persistentId)
	{
		pugi::xml_document doc;
		if (!doc.load_file(saveInfo.c_str()))
			return false;
		pugi::xml_node info = doc.child("info");
		if (!info)
			return false;
		const std::wstring wanted = fmt::format(L"{:08x}", persistentId);
		const pugi::xml_node match = info.find_child([&wanted](const pugi::xml_node& node) {
			return boost::iequals(node.attribute("persistentId").as_string(), wanted);
		});
		if (!match)
			return false;
		info.remove_child(match);
		return doc.save_file(saveInfo.c_str());
	}

	std::string GenericUtf8(const fs::path& path)
	{
		const auto text = path.generic_u8string();
		return std::string((const char*)text.data(), text.size());
	}

	void ShowZipError(wxWindow* parent, const wxString& what, const char* detail)
	{
		wxMessageBox(formatWxString(what, detail), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, parent);
	}

	TitleManager* CallbackOwner()
	{
		return static_cast<TitleManager*>(DownloadManager::GetInstance()->getUserData());
	}
}

TitleManager::TitleManager(wxWindow* parent, TitleManagerPage default_page)
	: wxFrame(parent, wxID_ANY, _("Title Manager"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE | wxTAB_TRAVERSAL)
{
	SetIcon(wxICON(X_BOX));

	m_notebook = new wxNotebook(this, wxID_ANY);
	m_notebook->AddPage(CreateTitleManagerPage(), _("Title Manager"), default_page == TitleManagerPage::TitleManager);
	m_notebook->AddPage(CreateDownloadManagerPage(), _("Download Manager"), default_page == TitleManagerPage::DownloadManager);

	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(m_notebook, 1, wxEXPAND);

	m_status_bar = CreateStatusBar(2, wxSTB_SIZEGRIP);
	m_status_bar->SetStatusText(_("Searching for titles..."));

	SetSizerAndFit(layout);
	Centre(wxBOTH);

	Bind(wxEVT_SET_STATUS_BAR_TEXT, &TitleManager::OnSetStatusBarText, this);
	Bind(wxEVT_TITLE_FOUND, &TitleManager::OnTitleFound, this);
	Bind(wxEVT_TITLE_SEARCH_COMPLETE, &TitleManager::OnTitleSearchComplete, this);
	Bind(wxEVT_DL_TITLE_UPDATE, &TitleManager::OnDownloadableTitleUpdate, this);
	Bind(wxEVT_DL_DISCONNECT_COMPLETE, &TitleManager::OnDisconnect, this);

	// reopening the window while the download manager is still connected picks the session back up
	if (DownloadManager* downloads = DownloadManager::GetInstance(false); downloads && downloads->IsConnected())
	{
		downloads->setUserData(this);
		downloads->registerCallbacks(Callback_ConnectStatusUpdate, Callback_AddDownloadableTitle, Callback_RemoveDownloadableTitle);
		SetConnected(true);
	}

	m_callbackId = CafeTitleList::RegisterCallback([](CafeTitleListCallbackEvent* evt, void* self) {
		static_cast<TitleManager*>(self)->HandleTitleListCallback(evt);
	}, this);
}

TitleManager::~TitleManager()
{
	CafeTitleList::UnregisterCallback(m_callbackId);

	if (DownloadManager* downloads = DownloadManager::GetInstance(false))
	{
		downloads->setUserData(nullptr);
		downloads->registerCallbacks(nullptr, nullptr, nullptr);

		// said once per run: closing the window does not stop downloads
		static bool s_toldAboutBackgroundDownloads = false;
		if (downloads->hasActiveDownloads() && !s_toldAboutBackgroundDownloads)
		{
			s_toldAboutBackgroundDownloads = true;
			wxMessageBox(_("Currently active downloads will continue in the background."), _("Information"), wxOK | wxCENTRE, this);
		}
	}

	m_running = false;
}

void TitleManager::SetFocusAndTab(TitleManagerPage page)
{
	m_notebook->SetSelection((int)page);
	SetFocus();
}

void TitleManager::SetDownloadStatusText(const wxString& text)
{
	auto* event = new wxCommandEvent(wxEVT_SET_TEXT);
	event->SetEventObject(m_status_text);
	event->SetString(text);
	wxQueueEvent(this, event);
}

// ---------------------------------------------------------------- installed titles tab

wxPanel* TitleManager::CreateTitleManagerPage()
{
	auto* page = new wxPanel(m_notebook);
	auto* column = new wxBoxSizer(wxVERTICAL);
	const wxColour iconColour = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);

	// filter row: label, text, refresh, prefix help
	auto* filterRow = new wxFlexGridSizer(0, 4, 0, 0);
	filterRow->AddGrowableCol(1);
	filterRow->Add(new wxStaticText(page, wxID_ANY, _("Filter")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	m_filter = new wxTextCtrl(page, wxID_ANY);
	m_filter->Bind(wxEVT_TEXT, &TitleManager::OnFilterChanged, this);
	filterRow->Add(m_filter, 1, wxALL | wxEXPAND, 5);

	const wxImage refreshIcon = wxHelper::LoadThemedBitmapFromPNG(PNG_REFRESH_png, sizeof(PNG_REFRESH_png), iconColour).ConvertToImage();
	m_refresh_button = new wxBitmapButton(page, wxID_ANY, refreshIcon.Scale(16, 16));
	m_refresh_button->SetToolTip(_("Refresh"));
	m_refresh_button->Disable(); // until the first scan is done
	m_refresh_button->Bind(wxEVT_BUTTON, &TitleManager::OnRefreshButton, this);
	filterRow->Add(m_refresh_button, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	auto* prefixHelp = new wxStaticBitmap(page, wxID_ANY, wxHelper::LoadThemedBitmapFromPNG(PNG_HELP_png, sizeof(PNG_HELP_png), iconColour));
	prefixHelp->SetToolTip(formatWxString(_("The following prefixes are supported:\n{0}\n{1}\n{2}\n{3}\n{4}"),
		"titleid:", "name:", "type:", "version:", "region:"));
	filterRow->Add(prefixHelp, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
	column->Add(filterRow, 0, wxEXPAND, 5);

	column->Add(new wxStaticLine(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL), 0, wxEXPAND | wxALL, 5);

	m_title_list = new wxTitleManagerList(page);
	m_title_list->SetSizeHints(800, 600);
	m_title_list->Bind(wxEVT_LIST_ITEM_SELECTED, &TitleManager::OnTitleSelected, this);
	column->Add(m_title_list, 1, wxALL | wxEXPAND, 5);

	column->Add(new wxStaticLine(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL), 0, wxEXPAND | wxALL, 5);

	// bottom row: install button | save tools (only shown for save entries)
	auto* bottomRow = new wxFlexGridSizer(0, 3, 0, 0);
	bottomRow->AddGrowableCol(2);

	auto* install = new wxButton(page, wxID_ANY, _("Install title"));
	install->Bind(wxEVT_BUTTON, &TitleManager::OnInstallTitle, this);
	bottomRow->Add(install, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
	bottomRow->Add(new wxStaticLine(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL), 0, wxEXPAND | wxALL, 5);

	m_save_panel = new wxPanel(page);
	auto* saveRow = new wxFlexGridSizer(0, 7, 0, 0);
	saveRow->Add(new wxStaticText(m_save_panel, wxID_ANY, _("Account")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	m_save_account_list = new wxChoice(m_save_panel, wxID_ANY);
	m_save_account_list->SetMinSize({ 170, -1 });
	m_save_account_list->Bind(wxEVT_CHOICE, &TitleManager::OnSaveAccountSelected, this);
	saveRow->Add(m_save_account_list, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

	// the account dependent buttons start disabled; Import works without an account
	auto addSaveButton = [&](const wxString& label, const wxString& tip, void (TitleManager::*handler)(wxCommandEvent&), bool needsAccount, int border) {
		auto* button = new wxButton(m_save_panel, wxID_ANY, label);
		button->Bind(wxEVT_BUTTON, handler, this);
		button->SetToolTip(tip);
		if (needsAccount)
			button->Disable();
		saveRow->Add(button, 0, wxALIGN_CENTER_VERTICAL | border, 5);
		return button;
	};
	addSaveButton(_("Open directory"), _("Open the directory of the save entry"), &TitleManager::OnSaveOpenDirectory, true, wxALL);
	addSaveButton(_("Transfer"), _("Transfers the save entry to another persistent account id"), &TitleManager::OnSaveTransfer, true, wxALL);
	addSaveButton(_("Delete"), _("Permanently delete the save entry"), &TitleManager::OnSaveDelete, true, wxALL);
	m_save_import = addSaveButton(_("Import"), _("Imports a zipped save entry"), &TitleManager::OnSaveImport, false, wxLEFT | wxTOP | wxBOTTOM);
	addSaveButton(_("Export"), _("Exports the selected save entry as zip file"), &TitleManager::OnSaveExport, true, wxRIGHT | wxTOP | wxBOTTOM);

	m_save_panel->SetSizerAndFit(saveRow);
	m_save_panel->Hide();
	bottomRow->Add(m_save_panel, 1, wxRESERVE_SPACE_EVEN_IF_HIDDEN | wxALIGN_CENTER_VERTICAL, 0);
	column->Add(bottomRow, 0, wxEXPAND, 5);

	page->SetSizerAndFit(column);
	return page;
}

void TitleManager::HandleTitleListCallback(CafeTitleListCallbackEvent* evt)
{
	if (evt->eventType == CafeTitleListCallbackEvent::TYPE::SCAN_FINISHED)
		wxQueueEvent(this, new wxCommandEvent(wxEVT_TITLE_SEARCH_COMPLETE));
}

void TitleManager::OnTitleFound(wxCommandEvent& event)
{
	auto* data = dynamic_cast<wxTitleManagerList::TitleEntryData_t*>(event.GetClientObject());
	wxASSERT(data);
	m_title_list->AddTitle(data);
}

void TitleManager::OnTitleSearchComplete(wxCommandEvent& event)
{
	m_isScanning = false;
	if (m_connectRequested)
	{
		m_connectRequested = false;
		InitiateConnect();
	}

	m_title_list->SortEntries(-1);
	const auto count = [this](wxTitleManagerList::EntryType type) { return m_title_list->GetCountByType(type); };
	m_status_bar->SetStatusText(formatWxString(_("Found {0} games, {1} updates, {2} DLCs and {3} save entries"),
		count(wxTitleManagerList::EntryType::Base) + count(wxTitleManagerList::EntryType::System),
		count(wxTitleManagerList::EntryType::Update),
		count(wxTitleManagerList::EntryType::Dlc),
		count(wxTitleManagerList::EntryType::Save)));

	const wxString filter = m_filter->GetValue();
	if (!filter.IsEmpty())
		m_title_list->Filter(filter);

	m_title_list->AutosizeColumns();
	m_refresh_button->Enable();
}

void TitleManager::OnSetStatusBarText(wxSetStatusBarTextEvent& event)
{
	m_status_bar->SetStatusText(event.GetText(), event.GetNumber());
}

void TitleManager::OnFilterChanged(wxCommandEvent& event)
{
	event.Skip();
	m_title_list->Filter(m_filter->GetValue());
}

void TitleManager::OnRefreshButton(wxCommandEvent& event)
{
	// the list is kept: the rescan reports changes one by one
	m_refresh_button->Disable();
	m_isScanning = true;
	m_status_bar->SetStatusText(_("Searching for titles..."));
	CafeTitleList::Refresh();
}

void TitleManager::OnInstallTitle(wxCommandEvent& event)
{
	wxFileDialog picker(this, _("Select title to install"), "", "", "meta.xml|meta.xml", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (picker.ShowModal() == wxID_CANCEL || picker.GetPath().IsEmpty())
		return;

	// <title>/meta/meta.xml -> <title>
	const fs::path titleFolder = fs::path(picker.GetPath().wc_string()).parent_path().parent_path();
	try
	{
		GameUpdateWindow installer(*this, titleFolder);
		if (installer.ShowModal() == wxID_OK)
		{
			CafeTitleList::AddTitleFromPath(installer.GetTargetPath());
			return;
		}
		if (!installer.GetExceptionMessage().empty())
			throw std::runtime_error(installer.GetExceptionMessage());
		wxMessageBox(_("Update installation has been canceled!"));
	}
	catch (const AbortException&)
	{
		// the user backed out of the window
	}
	catch (const std::exception& ex)
	{
		wxMessageBox(ex.what(), _("Update error"));
	}
}

void TitleManager::OnTitleSelected(wxListEvent& event)
{
	event.Skip();

	const auto entry = m_title_list->GetSelectedTitleEntry();
	if (!entry.has_value() || entry->type != wxTitleManagerList::EntryType::Save)
	{
		m_save_panel->Hide();
		return;
	}

	m_save_panel->Show();
	m_save_account_list->Clear();
	LoadSavePersistentIds(*entry);

	// nothing but Import works before an account is picked
	for (wxWindow* child : m_save_panel->GetChildren())
	{
		if (dynamic_cast<wxButton*>(child) && child->GetId() != m_save_import->GetId())
			child->Disable();
	}

	for (const uint32 id : entry->persistent_ids)
		AddSaveAccount(id);
}

void TitleManager::AddSaveAccount(uint32 persistentId)
{
	m_save_account_list->Append(AccountLabel(persistentId), (void*)(uintptr_t)persistentId);
}

void TitleManager::OnSaveAccountSelected(wxCommandEvent& event)
{
	event.Skip();
	for (wxWindow* child : m_save_panel->GetChildren())
	{
		if (!child->IsEnabled())
			child->Enable();
	}
}

void TitleManager::OnSaveOpenDirectory(wxCommandEvent& event)
{
	const int selected = m_save_account_list->GetSelection();
	if (selected == wxNOT_FOUND)
		return;
	const auto entry = m_title_list->GetSelectedTitleEntry();
	if (!entry.has_value())
		return;

	const fs::path folder = SaveAccountFolder(entry->title_id, (uint32)(uintptr_t)m_save_account_list->GetClientData(selected));
	if (IsExistingFolder(folder))
		wxLaunchDefaultApplication(wxHelper::FromPath(folder));
}

void TitleManager::OnSaveDelete(wxCommandEvent& event)
{
	const int selected = m_save_account_list->GetSelection();
	if (selected == wxNOT_FOUND)
		return;
	const wxString accountText = m_save_account_list->GetStringSelection();
	if (accountText.IsEmpty())
		return;

	const wxString question = formatWxString(_("Are you really sure that you want to delete the save entry for {}"), accountText);
	if (wxMessageBox(question, _("Warning"), wxYES_NO | wxCENTRE | wxICON_EXCLAMATION, this) == wxNO)
		return;

	auto entry = m_title_list->GetSelectedTitleEntry();
	if (!entry.has_value())
		return;
	const uint32 persistentId = (uint32)(uintptr_t)m_save_account_list->GetClientData(selected);
	const fs::path folder = SaveAccountFolder(entry->title_id, persistentId);
	if (!IsExistingFolder(folder))
		return;

	const fs::path saveInfo = SaveInfoFile(entry->title_id);
	std::error_code ec;
	if (!fs::exists(saveInfo, ec) || !RemoveFromSaveInfo(saveInfo, persistentId))
		cemuLog_log(LogType::Force, "TitleManager::OnSaveDelete: couldn't delete save entry in saveinfo.xml: {}", _pathToUtf8(saveInfo));

	std::erase(entry->persistent_ids, persistentId);

	fs::remove_all(folder, ec);
	if (ec)
	{
		wxMessageBox(formatWxString(_("Error when trying to delete the save directory:\n{}"), GetSystemErrorMessage(ec)), _("Error"), wxOK | wxCENTRE, this);
		return;
	}
	m_save_account_list->Delete(selected);
}

void TitleManager::OnSaveTransfer(wxCommandEvent& event)
{
	const int selected = m_save_account_list->GetSelection();
	if (selected == wxNOT_FOUND)
		return;
	const wxString accountText = m_save_account_list->GetStringSelection();
	if (accountText.IsEmpty())
		return;
	auto entry = m_title_list->GetSelectedTitleEntry();
	if (!entry.has_value())
		return;

	const uint32 fromId = (uint32)(uintptr_t)m_save_account_list->GetClientData(selected);
	SaveTransfer transfer(this, entry->title_id, accountText, fromId);
	if (transfer.ShowModal() == wxCANCEL)
		return;

	auto& ids = entry->persistent_ids;
	std::erase(ids, fromId);
	const uint32 toId = transfer.GetTargetPersistentId();
	if (toId != 0 && std::find(ids.cbegin(), ids.cend(), toId) == ids.cend())
	{
		ids.emplace_back(toId);
		AddSaveAccount(toId);
	}
	m_save_account_list->Delete(selected);
}

void TitleManager::OnSaveExport(wxCommandEvent& event)
{
	const int selected = m_save_account_list->GetSelection();
	if (selected == wxNOT_FOUND || m_save_account_list->GetStringSelection().IsEmpty())
		return;
	const auto entry = m_title_list->GetSelectedTitleEntry();
	if (!entry.has_value())
		return;
	const uint32 persistentId = (uint32)(uintptr_t)m_save_account_list->GetClientData(selected);

	wxFileDialog picker(this, _("Select a target file to export the save entry"), wxHelper::FromPath(entry->path), wxEmptyString,
		fmt::format("{}|*.zip", _("Exported save entry (*.zip)")), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (picker.ShowModal() != wxID_OK || picker.GetPath().IsEmpty())
		return;

	int openError;
	zip_t* zip = zip_open(picker.GetPath().ToUTF8().data(), ZIP_CREATE | ZIP_TRUNCATE, &openError);
	if (!zip)
	{
		zip_error_t error;
		zip_error_init_with_code(&error, openError);
		ShowZipError(this, _("Error when creating the zip for the save entry:\n{}"), zip_error_strerror(&error));
		zip_error_fini(&error);
		return;
	}

	// every file and folder below user/<id>, stored relative to it
	const fs::path accountFolder = fs::path(entry->path) / fmt::format("user/{:08x}", persistentId);
	const std::string prefix = GenericUtf8(accountFolder);
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(accountFolder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
	{
		const std::string fullName = GenericUtf8(it->path());
		const std::string zipName = fullName.substr(prefix.size() + 1);
		if (it->is_directory(ec))
		{
			if (zip_dir_add(zip, zipName.c_str(), ZIP_FL_ENC_UTF_8) < 0)
				ShowZipError(this, _("Error when trying to add a directory to the zip:\n{}"), zip_strerror(zip));
			continue;
		}
		zip_source_t* source = zip_source_file(zip, fullName.c_str(), 0, 0);
		if (!source)
		{
			ShowZipError(this, _("Error when trying to add a file to the zip:\n{}"), zip_strerror(zip));
			continue;
		}
		if (zip_file_add(zip, zipName.c_str(), source, ZIP_FL_ENC_UTF_8) < 0)
		{
			ShowZipError(this, _("Error when trying to add a file to the zip:\n{}"), zip_strerror(zip));
			zip_source_free(source);
		}
	}

	if (ec)
		cemuLog_log(LogType::Force, "TitleManager: save export could not list {}: {}", _pathToUtf8(accountFolder), ec.message());

	// the import side checks this marker against the title it is imported into
	const std::string marker = fmt::format("titleId = {:#016x}", entry->title_id);
	zip_source_t* markerSource = zip_source_buffer(zip, marker.data(), marker.size(), 0);
	if (zip_file_add(zip, "cemu_meta", markerSource, ZIP_FL_ENC_UTF_8) < 0)
	{
		ShowZipError(this, _("Error when trying to add cemu_meta file to the zip:\n{}"), zip_strerror(zip));
		zip_source_free(markerSource);
	}
	// writes the archive; the marker buffer must still be alive here
	zip_close(zip);
}

void TitleManager::OnSaveImport(wxCommandEvent& event)
{
	auto entry = m_title_list->GetSelectedTitleEntry();
	if (!entry.has_value())
		return;

	SaveImportWindow import(this, entry->title_id);
	if (import.ShowModal() == wxCANCEL)
		return;

	auto& ids = entry->persistent_ids;
	const uint32 newId = import.GetTargetPersistentId();
	if (newId != 0 && std::find(ids.cbegin(), ids.cend(), newId) == ids.cend())
	{
		ids.emplace_back(newId);
		AddSaveAccount(newId);
	}
}

// ---------------------------------------------------------------- download manager tab

wxPanel* TitleManager::CreateDownloadManagerPage()
{
	auto* page = new wxPanel(m_notebook);
	auto* column = new wxBoxSizer(wxVERTICAL);

	auto* connectRow = new wxBoxSizer(wxHORIZONTAL);
#if DOWNLOADMGR_HAS_ACCOUNT_DROPDOWN
	m_account = new wxChoice(page, wxID_ANY);
	m_account->SetMinSize({ 250, -1 });
	const uint32 activeId = GetConfig().account.m_persistent_id.GetValue();
	for (const Account& account : Account::GetAccounts())
	{
		m_account->Append(account.ToString(), (void*)(uintptr_t)account.GetPersistentId());
		if (account.GetPersistentId() == activeId)
			m_account->SetSelection(m_account->GetCount() - 1);
	}
	connectRow->Add(m_account, 0, wxALL, 5);
#endif
	m_connect = new wxButton(page, wxID_ANY, _("Connect"));
	m_connect->Bind(wxEVT_BUTTON, &TitleManager::OnConnect, this);
	connectRow->Add(m_connect, 0, wxALL, 5);
	column->Add(connectRow, 0, wxEXPAND, 5);

#if DOWNLOADMGR_HAS_ACCOUNT_DROPDOWN
	m_status_text = new wxStaticText(page, wxID_ANY, _("Select an account and press Connect"));
#else
	if (NCrypto::HasDataForConsoleCert())
	{
		m_status_text = new wxStaticText(page, wxID_ANY, _("Click on Connect to load the list of downloadable titles"));
	}
	else
	{
		m_status_text = new wxStaticText(page, wxID_ANY, _("Valid online files are required to download eShop titles. For more information, go to the Account tab in the General Settings."));
		m_connect->Enable(false);
	}
#endif
	Bind(wxEVT_SET_TEXT, &TitleManager::OnSetStatusText, this);
	column->Add(m_status_text, 0, wxALL, 5);

	column->Add(new wxStaticLine(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_HORIZONTAL), 0, wxEXPAND | wxALL, 5);

	// list filters, usable once connected
	auto* filterRow = new wxFlexGridSizer(0, 3, 0, 0);
	auto addFilter = [&](const wxString& label) {
		auto* box = new wxCheckBox(page, wxID_ANY, label);
		box->SetValue(true);
		box->Enable(false);
		box->Bind(wxEVT_CHECKBOX, &TitleManager::OnDlFilterCheckbox, this);
		filterRow->Add(box, 0, wxALL, 5);
		return box;
	};
	m_show_titles = addFilter(_("Show available titles"));
	m_show_updates = addFilter(_("Show available updates"));
	m_show_installed = addFilter(_("Show installed"));
	column->Add(filterRow, 0, wxEXPAND, 5);

	m_download_list = new wxDownloadManagerList(page);
	m_download_list->SetSizeHints(800, 600);
	m_download_list->Bind(wxEVT_LIST_ITEM_SELECTED, &TitleManager::OnTitleSelected, this);
	column->Add(m_download_list, 1, wxALL | wxEXPAND, 5);

	page->SetSizerAndFit(column);
	return page;
}

void TitleManager::InitiateConnect()
{
#if DOWNLOADMGR_HAS_ACCOUNT_DROPDOWN
	const Account& account = Account::GetAccount((uint32)(uintptr_t)m_account->GetClientData(m_account->GetSelection()));
#endif
	DownloadManager* downloads = DownloadManager::GetInstance();
	downloads->reset();
	m_download_list->SetCurrentDownloadMgr(downloads);

	const std::string deviceCert = NCrypto::CertECC::GetDeviceCertificate().encodeToBase64();
	if (!NCrypto::SEEPROM_IsPresent())
	{
		SetDownloadStatusText(_("Dumped online files not found"));
		return;
	}

	SetDownloadStatusText(_("Connecting..."));
	downloads->setUserData(this);
	downloads->registerCallbacks(Callback_ConnectStatusUpdate, Callback_AddDownloadableTitle, Callback_RemoveDownloadableTitle);

	std::string accountName;
	std::array<uint8, 32> accountPassword;
	std::string accountCountry;
#if DOWNLOADMGR_HAS_ACCOUNT_DROPDOWN
	accountName = account.GetAccountId();
	accountPassword = account.GetAccountPasswordCache();
	accountCountry.assign(NCrypto::GetCountryAsString(account.GetCountry()));
#endif
	downloads->connect(accountName, accountPassword, NCrypto::SEEPROM_GetRegion(), accountCountry, NCrypto::GetDeviceId(), NCrypto::GetSerial(), deviceCert);
}

void TitleManager::OnConnect(wxCommandEvent& event)
{
	if (m_isScanning)
	{
		// installed state is needed for the list; connect once the scan reports back
		SetDownloadStatusText(_("Getting installed title information..."));
		m_connectRequested = true;
	}
	else
		InitiateConnect();
	SetConnected(true);
}

void TitleManager::SetConnected(bool state)
{
#if DOWNLOADMGR_HAS_ACCOUNT_DROPDOWN
	m_account->Enable(!state);
#endif
	m_connect->Enable(!state);
	for (wxWindow* control : { (wxWindow*)m_show_titles, (wxWindow*)m_show_updates, (wxWindow*)m_show_installed, (wxWindow*)m_download_list })
		control->Enable(state);
}

void TitleManager::OnDlFilterCheckbox(wxCommandEvent& event)
{
	m_download_list->Filter2(m_show_titles->GetValue(), m_show_updates->GetValue(), m_show_installed->GetValue());
	m_download_list->SortEntries();
}

void TitleManager::OnSetStatusText(wxCommandEvent& event)
{
	auto* control = wxDynamicCast(event.GetEventObject(), wxControl);
	wxASSERT(control);
	control->SetLabel(event.GetString());
}

void TitleManager::OnDownloadableTitleUpdate(wxCommandEvent& event)
{
	if (auto* data = dynamic_cast<wxDownloadManagerList::TitleEntryData_t*>(event.GetClientObject()))
		m_download_list->AddOrUpdateTitle(data); // copies the entry; the event keeps ownership
}

void TitleManager::OnDisconnect(wxCommandEvent& event)
{
	SetConnected(false);
}

void TitleManager::Callback_ConnectStatusUpdate(std::string statusText, DLMGR_STATUS_CODE statusCode)
{
	TitleManager* window = CallbackOwner();
	if (!window)
		return;
	window->SetDownloadStatusText(wxString::FromUTF8(statusText));
	if (statusCode == DLMGR_STATUS_CODE::FAILED)
		wxQueueEvent(window, new wxCommandEvent(wxEVT_DL_DISCONNECT_COMPLETE)); // re-enables Connect
}

void TitleManager::Callback_AddDownloadableTitle(const DlMgrTitleReport& titleInfo)
{
	TitleManager* window = CallbackOwner();
	if (!window)
		return;
	auto* event = new wxCommandEvent(wxEVT_DL_TITLE_UPDATE);
	event->SetClientObject(new wxDownloadManagerList::TitleEntryData_t(MakeDownloadEntry(titleInfo)));
	wxQueueEvent(window, event);
}

void TitleManager::Callback_RemoveDownloadableTitle(uint64 titleId, uint16 version)
{
	// the download manager never removes titles from the list
	assert_dbg();
}
