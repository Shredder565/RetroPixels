// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/TitleList/GameInfo.h"

#include <wx/dialog.h>
#include <wx/timer.h>
#include <wx/gauge.h>

#include <atomic>
#include <string>
#include <array>
#include <memory>

// thrown when the user decides not to (re)install a title after all
class AbortException : public std::exception {};

// Installs a title folder (code/content/meta: a game, update or DLC) into the mlc folder.
// The constructor checks the title and asks about replacing what is installed (throwing
// AbortException on "no"); the copy then runs on a thread while the dialog shows its progress.
// A title that was there before is kept as <folder>.backup until the copy has finished, and put
// back if it is cancelled or fails.
class GameUpdateWindow : public wxDialog
{
public:
	GameUpdateWindow(wxWindow& parent, const fs::path& metaPath);
	~GameUpdateWindow();

	uint64 GetTitleId() const { return m_title_info.GetAppTitleId(); }
	bool HasException() const { return !m_thread_exception.empty(); }
	const std::string& GetExceptionMessage() const { return m_thread_exception; }
	const std::string GetGameName() const { return m_title_info.GetMetaTitleName(); }
	uint32 GetTargetVersion() const { return m_title_info.GetAppTitleVersion(); }
	fs::path GetTargetPath() const { return fs::path(m_target_path); }

	// wxID_OK once everything was copied, wxID_CANCEL otherwise
	int ShowModal() override;
	void OnClose(wxCloseEvent& event);

	void OnUpdate(const wxTimerEvent& event);
	void OnCancelButton(const wxCommandEvent& event);

private:
	TitleInfo m_title_info;
	enum ThreadState_t
	{
		ThreadRunning,
		ThreadCanceled,
		ThreadFinished,
	};

	uint64_t m_required_size;
	fs::path m_target_path;
	std::array<fs::path, 3> m_source_paths; // content, code, meta
	bool ParseUpdate(const fs::path& metaPath);

	std::atomic<uint64> m_processed_size = 0;
	std::atomic<ThreadState_t> m_thread_state;
	std::string m_thread_exception;
	std::thread m_thread;
	void ThreadWork();

	fs::path m_backup_folder; // the previously installed title while the copy runs

	wxGauge* m_processBar;
	wxTimer* m_timer;
};
