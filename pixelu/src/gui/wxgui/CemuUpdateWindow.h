// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/dialog.h>
#include <wx/stattext.h>
#include <wx/gauge.h>
#include <wx/timer.h>
#include <wx/hyperlink.h>
#include <wx/checkbox.h>

#include <future>

// "Check for updates". PixelU has no updater of its own: it is updated together with RetroPixels
// (Cemu's updater would fetch and install Cemu builds, so it is not used). The window only says so,
// and the update check never reports an update.
class CemuUpdateWindow : public wxDialog
{
public:
	CemuUpdateWindow(wxWindow* parent);
	~CemuUpdateWindow();

	// an already finished check that found nothing
	static std::future<bool> IsUpdateAvailableAsync();

private:
	void OnCloseButton(wxCommandEvent& event);
};
