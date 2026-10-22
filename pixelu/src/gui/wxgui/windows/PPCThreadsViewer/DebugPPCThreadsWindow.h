// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/wx.h>

class wxListView;

// Debug window listing the guest's active OS threads (refreshed every 250 ms
// while "Auto refresh" is ticked). The context menu changes a thread's priority,
// suspends/resumes it, logs its stack trace or samples its PC for a profile.
class DebugPPCThreadsWindow : public wxFrame
{
public:
	DebugPPCThreadsWindow(wxFrame& parent);
	~DebugPPCThreadsWindow();

	void OnCloseButton(wxCommandEvent& event);
	void OnRefreshButton(wxCommandEvent& event);
	void OnClose(wxCloseEvent& event);
	void RefreshThreadList();
	void OnThreadListPopupClick(wxCommandEvent& evt);
	void OnThreadListRightClick(wxMouseEvent& event);

	void Close();

private:
	void ProfileThread(struct OSThread_t* thread);
	void ProfileThreadWorker(OSThread_t* thread);
	void PresentProfileResults(OSThread_t* thread, const std::unordered_map<VAddr, uint32>& samples);
	void DumpStackTrace(struct OSThread_t* thread);
	void FillRow(long row, uint32 threadAddr, OSThread_t* thread);
	void OnTimer(wxTimerEvent& event);

	wxListView* m_list;
	wxCheckBox* m_autoRefresh;
	wxTimer* m_refreshTimer;
};
