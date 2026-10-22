// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <atomic>
#include <thread>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/stattext.h>

// Searches for a Wiimote / Wii U Pro Controller over Bluetooth on a worker
// thread and pairs it (Windows) or registers its address (BlueZ).
class PairingDialog : public wxDialog
{
public:
	PairingDialog(wxWindow* parent);
	~PairingDialog();

private:
	enum class PairingState
	{
		Pairing,
		Finished,
		NoBluetoothAvailable,
		SearchFailed,
		PairingFailed,
		BluetoothUnusable,
	};

	void OnCloseWindow(wxCloseEvent& event);
	void OnStateEvent(wxCommandEvent& event);
	void ShowState(PairingState state);

	// worker thread
	void Search();
	void Report(PairingState state);

	wxStaticText* m_status;
	wxGauge* m_progress;
	wxButton* m_cancel;

	std::thread m_worker;
	std::atomic<bool> m_stop{false};
};
