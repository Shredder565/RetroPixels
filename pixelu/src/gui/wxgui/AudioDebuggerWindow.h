// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/wx.h>

class wxListCtrl;

// Debug > View audio debugger: a live table of the AX voices (refreshed every 100 ms).
class AudioDebuggerWindow : public wxFrame
{
public:
	AudioDebuggerWindow(wxFrame& parent);

	void OnCloseButton(wxCommandEvent& event);
	void OnRefreshButton(wxCommandEvent& event);
	void OnClose(wxCloseEvent& event);
	void RefreshVoiceList_sndgeneric();
	void RefreshVoiceList();
	void OnRefreshTimer(wxTimerEvent& event);
	void OnVoiceListPopupClick(wxCommandEvent& evt);
	void OnVoiceListRightClick(wxMouseEvent& event);

	// stops refreshing and destroys the window
	void Close();

private:
	wxListCtrl* voiceListbox;
	wxTimer* refreshTimer;
};
