// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/wx.h>
#include "wxCemuConfig.h"
#include "input/api/Controller.h"

// Global hotkeys (fullscreen, screenshot, fast-forward, quit) for keyboard and
// controller, and the window that edits them. Init() builds the lookup tables
// from the config; CaptureInput() is fed every key press / controller poll.
class HotkeySettings : public wxFrame
{
public:
	static void Init(class MainWindow* mainWindow);

	static void CaptureInput(wxKeyEvent& event);
	static void CaptureInput(const ControllerState& currentState, const ControllerState& lastState);

	HotkeySettings(wxWindow* parent);
	~HotkeySettings();

private:
	enum class Device
	{
		Keyboard,
		Controller,
	};

	wxButton* AddRow(const wxString& label, sHotkeyCfg& hotkey);
	wxString KeyboardText(uKeyboardHotkey hotkey) const;
	wxString ControllerText(ControllerHotkey_t hotkey) const;
	bool PickController();

	void BeginCapture(wxButton* button, Device device);
	void EndCapture();
	void ShowBinding(wxButton* button, Device device);
	void ClearBinding(wxButton* button, Device device);

	void OnCaptureKeyUp(wxKeyEvent& event);
	void OnControllerPoll(wxTimerEvent& event);

	wxPanel* m_panel;
	wxFlexGridSizer* m_grid;
	wxTimer* m_controllerPoll{nullptr};

	wxButton* m_capturing{nullptr}; // button waiting for input, if any
	Device m_capturingDevice = Device::Keyboard;

	std::weak_ptr<ControllerBase> m_controller{}; // labels and capture use this controller
	bool m_dirty = false;
};
