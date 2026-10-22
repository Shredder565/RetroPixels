// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <thread>
#include <atomic>

#include <wx/frame.h>
#include <wx/button.h>

#include "Cemu/Tools/DownloadManager/DownloadManager.h"

// 0: the download manager connects with the console's own online files only (no account picker)
#define DOWNLOADMGR_HAS_ACCOUNT_DROPDOWN 0

class wxCheckBox;
class wxStaticText;
class wxListEvent;
class wxSetStatusBarTextEvent;
class wxTitleManagerList;
class wxDownloadManagerList;
class wxTextCtrl;
class wxStatusBar;
class wxImageList;
class wxBitmapButton;
class wxPanel;
class wxChoice;
class wxNotebook;

// the notebook tabs, in page order
enum class TitleManagerPage
{
	TitleManager = 0,
	DownloadManager = 1
};

enum class DLMGR_STATUS_CODE;

// Tools > Title Manager: one tab lists the installed games, updates, DLC and saves (with save
// management for the selected account), the other is the eShop download manager.
class TitleManager : public wxFrame
{
public:
	TitleManager(wxWindow* parent, TitleManagerPage default_page = TitleManagerPage::TitleManager);
	~TitleManager();

	// brings the window forward on the given tab
	void SetFocusAndTab(TitleManagerPage page);

	// thread safe: the text is applied on the GUI thread
	void SetDownloadStatusText(const wxString& text);

private:
	// --- installed titles tab
	wxPanel* CreateTitleManagerPage();
	void HandleTitleListCallback(struct CafeTitleListCallbackEvent* evt);
	void OnTitleFound(wxCommandEvent& event);
	void OnTitleSearchComplete(wxCommandEvent& event);
	void OnSetStatusBarText(wxSetStatusBarTextEvent& event);
	void OnFilterChanged(wxCommandEvent& event);
	void OnRefreshButton(wxCommandEvent& event);
	void OnInstallTitle(wxCommandEvent& event);
	void OnTitleSelected(wxListEvent& event);

	// save entry of the selected title, for the account picked in m_save_account_list
	void OnSaveAccountSelected(wxCommandEvent& event);
	void OnSaveOpenDirectory(wxCommandEvent& event);
	void OnSaveDelete(wxCommandEvent& event);
	void OnSaveTransfer(wxCommandEvent& event);
	void OnSaveExport(wxCommandEvent& event);
	void OnSaveImport(wxCommandEvent& event);
	void AddSaveAccount(uint32 persistentId);

	wxNotebook* m_notebook;
	uint32 m_callbackId;

	wxTextCtrl* m_filter;
	wxTitleManagerList* m_title_list;
	wxStatusBar* m_status_bar;
	wxBitmapButton* m_refresh_button;
	wxPanel* m_save_panel;
	wxChoice* m_save_account_list;
	wxButton* m_save_import;

	bool m_isScanning{ true }; // the title list is still being scanned
	std::atomic_bool m_running = true;

	// --- download manager tab
	wxPanel* CreateDownloadManagerPage();
	void InitiateConnect();
	void SetConnected(bool state);
	void OnConnect(wxCommandEvent& event);
	void OnSetStatusText(wxCommandEvent& event);
	void OnDownloadableTitleUpdate(wxCommandEvent& event);
	void OnDisconnect(wxCommandEvent& event);
	void OnDlFilterCheckbox(wxCommandEvent& event);

	// called by the download manager from its own thread
	static void Callback_ConnectStatusUpdate(std::string statusText, DLMGR_STATUS_CODE statusCode);
	static void Callback_AddDownloadableTitle(const struct DlMgrTitleReport& titleInfo);
	static void Callback_RemoveDownloadableTitle(uint64 titleId, uint16 version);

	wxChoice* m_account;
	wxButton* m_connect;
	wxStaticText* m_status_text;
	wxCheckBox *m_show_titles, *m_show_updates, *m_show_installed;
	wxDownloadManagerList* m_download_list;
	bool m_connectRequested{ false }; // Connect was pressed while the title scan was still running
};
