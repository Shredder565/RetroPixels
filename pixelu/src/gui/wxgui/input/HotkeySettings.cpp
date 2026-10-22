// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/HotkeySettings.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "interface/WindowSystem.h"
#include <config/ActiveSettings.h>
#include "input/InputManager.h"
#include "MainWindow.h"
#include "wxgui/RPEmbed.h"

#include <wx/app.h>
#include <wx/clipbrd.h>

#if BOOST_OS_WINDOWS
#include <ole2.h>
#endif

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif

extern WindowSystem::WindowInfo g_window_info;

// ---- screenshots ----

namespace
{
	// ==== shotname begin ====
	// Screenshot_YYYY-MM-DD_HH-MM-SS, with _GamePad for the gamepad view
	std::string ScreenshotStem(const std::tm& t, bool gamepad)
	{
		std::string stem = fmt::format("Screenshot_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
		if (gamepad)
			stem += "_GamePad";
		return stem;
	}

	// n-th candidate file name for a stem: stem.png, stem_2.png ... stem_999.png
	constexpr int kMaxScreenshotCandidates = 999;
	std::string ScreenshotCandidate(const std::string& stem, int n)
	{
		return n == 0 ? stem + ".png" : fmt::format("{}_{}.png", stem, n + 1);
	}
	// ==== shotname end ====

	std::optional<fs::path> FreeScreenshotPath(bool gamepad)
	{
		const fs::path folder = ActiveSettings::GetUserDataPath("screenshots");
		const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		const std::string stem = ScreenshotStem(*std::localtime(&now), gamepad);
		for (int n = 0; n < kMaxScreenshotCandidates; n++)
		{
			fs::path candidate = folder / ScreenshotCandidate(stem, n);
			std::error_code ec;
			const bool taken = fs::exists(candidate, ec);
			if (!ec && !taken)
				return candidate;
		}
		return std::nullopt;
	}

	bool WriteScreenshotFile(const fs::path& path, const wxImage& image)
	{
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		if (ec)
			return false;
		wxLogNull noPopup; // a failed save must not open a message box
		return image.SaveFile(path.wstring());
	}

	bool CopyScreenshotToClipboard(const wxImage& image)
	{
		static std::mutex s_clipboardLock;
		std::lock_guard lock(s_clipboardLock);
		if (!wxTheClipboard->Open())
			return false;
		// the clipboard may hand the picture out later, after the pixel buffer is gone
		wxTheClipboard->SetData(new wxImageDataObject(image.Copy()));
		wxTheClipboard->Close();
		return true;
	}

	// Called on a worker thread by the renderer. The TV picture goes to the
	// clipboard and, if enabled, to a file; the gamepad picture only to a file.
	// Returns the notification to show, if any.
	std::optional<std::string> SaveScreenshot(const std::vector<uint8>& rgb, int width, int height, bool mainWindow)
	{
#if BOOST_OS_WINDOWS
		// wx's clipboard goes through OLE, which needs initialising per thread
		OleInitialize(nullptr);
#endif
		const bool toFile = GetWxGUIConfig().save_screenshot;
		wxImage image(width, height, const_cast<uint8*>(rgb.data()), true);
		if (mainWindow)
		{
			if (!CopyScreenshotToClipboard(image))
				return _tr("Failed to open clipboard");
			if (!toFile)
				return _tr("Screenshot saved to clipboard");
		}
		if (!toFile)
			return std::nullopt;
		const auto path = FreeScreenshotPath(!mainWindow);
		if (!path || !WriteScreenshotFile(*path, image))
			return _tr("Failed to save screenshot to file");
		if (mainWindow)
			return _tr("Screenshot saved");
		return std::nullopt;
	}
}

// ---- hotkey tables ----

namespace
{
	using Action = std::function<void(void)>;

	MainWindow* s_mainWindow = nullptr;
	auto& s_cfgHotkeys = GetWxGUIConfig().hotkeys;
	// what each configured hotkey does; the modifier has no entry
	std::unordered_map<sHotkeyCfg*, Action> s_cfgHotkeyToFuncMap;
	// lookup by the bound key (raw uKeyboardHotkey) / controller button
	std::unordered_map<uint16, Action> s_keyActions;
	std::unordered_map<uint16, Action> s_buttonActions;

	sHotkeyCfg& HotkeyOf(wxButton* button)
	{
		return *static_cast<sHotkeyCfg*>(button->GetClientData());
	}

	uKeyboardHotkey KeyOf(const wxKeyEvent& event)
	{
		uKeyboardHotkey key{};
		key.key = event.GetKeyCode();
		key.alt = event.AltDown();
		key.ctrl = event.ControlDown();
		key.shift = event.ShiftDown();
		return key;
	}

	// keys that cannot be a hotkey on their own; Escape cancels the capture
	bool IsBindableKey(int keycode)
	{
		return keycode != WXK_NONE && keycode != WXK_ESCAPE && keycode != WXK_ALT && keycode != WXK_CONTROL && keycode != WXK_SHIFT;
	}

	void ToggleFullscreen()
	{
		s_mainWindow->SetFullScreen(!s_mainWindow->IsFullScreen());
	}

	const wxSize kCellMinSize{250, 45};
}

void HotkeySettings::Init(MainWindow* mainWindowFrame)
{
	auto bind = [](sHotkeyCfg& hotkey, Action action) { s_cfgHotkeyToFuncMap.emplace(&hotkey, std::move(action)); };
	bind(s_cfgHotkeys.toggleFullscreen, ToggleFullscreen);
	bind(s_cfgHotkeys.toggleFullscreenAlt, ToggleFullscreen);
	bind(s_cfgHotkeys.exitFullscreen, [] { s_mainWindow->SetFullScreen(false); });
	bind(s_cfgHotkeys.takeScreenshot, [] {
		if (g_renderer)
			g_renderer->RequestScreenshot(SaveScreenshot);
	});
	bind(s_cfgHotkeys.toggleFastForward, [] {
		// 3x speed or back to normal
		ActiveSettings::SetTimerShiftFactor(ActiveSettings::GetTimerShiftFactor() < 3 ? 3 : 1);
	});
	bind(s_cfgHotkeys.exitApplication, [] {
		auto* close = new wxCloseEvent(wxEVT_CLOSE_WINDOW, s_mainWindow->GetId());
		close->SetCanVeto(false);
		wxQueueEvent(s_mainWindow, close);
	});
#ifdef CEMU_DEBUG_ASSERT
	bind(s_cfgHotkeys.endEmulation, [] {
		wxTheApp->CallAfter([] { s_mainWindow->EndEmulation(); });
	});
#endif

	if (RPEmbed::Enabled())
	{
		// PixelU: the window belongs to RetroPixels then: hotkeys neither switch fullscreen nor quit the game
		for (sHotkeyCfg* hotkey : {&s_cfgHotkeys.toggleFullscreen, &s_cfgHotkeys.toggleFullscreenAlt, &s_cfgHotkeys.exitFullscreen, &s_cfgHotkeys.exitApplication})
			s_cfgHotkeyToFuncMap[hotkey] = [](void) {};
	}

	s_keyActions.reserve(s_cfgHotkeyToFuncMap.size());
	for (const auto& [hotkey, action] : s_cfgHotkeyToFuncMap)
	{
		if (hotkey->keyboard.raw > sHotkeyCfg::keyboardNone)
			s_keyActions[hotkey->keyboard.raw] = action;
		if (hotkey->controller > sHotkeyCfg::controllerNone)
			s_buttonActions[hotkey->controller] = action;
	}
	s_mainWindow = mainWindowFrame;
}

void HotkeySettings::CaptureInput(wxKeyEvent& event)
{
	const auto found = s_keyActions.find(KeyOf(event).raw);
	if (found != s_keyActions.end())
		found->second();
}

void HotkeySettings::CaptureInput(const ControllerState& currentState, const ControllerState& lastState)
{
	// controller hotkeys only work while the modifier button is held
	const ControllerHotkey_t modifier = s_cfgHotkeys.modifiers.controller;
	if (modifier < 0 || !currentState.buttons.GetButtonState(modifier))
		return;
	for (const uint32 button : currentState.buttons.GetButtonList())
	{
		const auto found = s_buttonActions.find(button);
		if (found == s_buttonActions.end())
			continue;
		// the first bound button decides; it fires on the press only
		if (!lastState.buttons.GetButtonState(button))
			found->second();
		return;
	}
}

// ---- settings window ----

HotkeySettings::HotkeySettings(wxWindow* parent)
	: wxFrame(parent, wxID_ANY, _("Hotkey Settings"))
{
	SetIcon(wxICON(X_HOTKEY_SETTINGS));

	m_grid = new wxFlexGridSizer(0, 3, 5, 5);
	m_grid->AddGrowableCol(1);
	m_grid->AddGrowableCol(2);
	m_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME);
	m_panel->SetSizer(m_grid);
	Center();

	PickController();

	// column titles
	{
		const auto headerFlags = wxSizerFlags().Expand().Border(wxTOP, 10);
		m_grid->Add(new wxStaticText(m_panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL), headerFlags);
		for (const wxString& title : {wxString(_tr("Keyboard")), wxString(_tr("Controller"))})
		{
			auto* header = new wxStaticText(m_panel, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
			header->SetFont(header->GetFont().Bold().Larger());
			header->SetMinSize(kCellMinSize);
			m_grid->Add(header, headerFlags);
		}
	}

	// the modifier is controller only
	AddRow(_tr("Hotkey modifier"), s_cfgHotkeys.modifiers)->Hide();
	AddRow(_tr("Toggle fullscreen"), s_cfgHotkeys.toggleFullscreen);
	AddRow(_tr("Take screenshot"), s_cfgHotkeys.takeScreenshot);
	AddRow(_tr("Toggle fast-forward"), s_cfgHotkeys.toggleFastForward);
#ifdef CEMU_DEBUG_ASSERT
	AddRow(_tr("End emulation"), s_cfgHotkeys.endEmulation);
#endif
	AddRow(_tr("Exit application"), s_cfgHotkeys.exitApplication);

	m_controllerPoll = new wxTimer(this);
	Bind(wxEVT_TIMER, &HotkeySettings::OnControllerPoll, this);

	m_grid->SetSizeHints(this);
}

HotkeySettings::~HotkeySettings()
{
	m_controllerPoll->Stop();
	if (m_dirty)
		GetConfigHandle().Save();
}

// returns the keyboard button of the new row
wxButton* HotkeySettings::AddRow(const wxString& label, sHotkeyCfg& hotkey)
{
	auto* caption = new wxStaticText(m_panel, wxID_ANY, label);
	auto* keyButton = new wxButton(m_panel, wxID_ANY, KeyboardText(hotkey.keyboard), wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBU_EXACTFIT);
	auto* padButton = new wxButton(m_panel, wxID_ANY, ControllerText(hotkey.controller), wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS | wxBU_EXACTFIT);

	const wxColour background = GetBackgroundColour();
	for (const auto& cell : {std::pair{keyButton, Device::Keyboard}, std::pair{padButton, Device::Controller}})
	{
		wxButton* const button = cell.first;
		const Device device = cell.second;
		button->SetClientData(&hotkey);
		button->SetMinSize(kCellMinSize);
		button->SetBackgroundColour(background);
		// left click starts capturing, right click cancels a capture or clears the binding
		button->Bind(wxEVT_BUTTON, [this, button, device](wxCommandEvent&) { BeginCapture(button, device); });
		button->Bind(wxEVT_RIGHT_UP, [this, button, device](wxMouseEvent&) {
			if (m_capturing)
				EndCapture();
			else
				ClearBinding(button, device);
		});
	}

	const auto cellFlags = wxSizerFlags(1).Expand().Border(wxALL, 5).CenterVertical();
	m_grid->Add(caption, cellFlags);
	m_grid->Add(keyButton, cellFlags);
	m_grid->Add(padButton, cellFlags);
	return keyButton;
}

wxString HotkeySettings::KeyboardText(uKeyboardHotkey hotkey) const
{
	if (hotkey.raw == sHotkeyCfg::keyboardNone)
		return _("----");
	wxString text;
	if (hotkey.alt)
		text << _("Alt + ");
	if (hotkey.ctrl)
		text << _("Ctrl + ");
	if (hotkey.shift)
		text << _("Shift + ");
	text << wxAcceleratorEntry(0, hotkey.key).ToString();
	return text;
}

wxString HotkeySettings::ControllerText(ControllerHotkey_t hotkey) const
{
	const auto controller = m_controller.lock();
	if (hotkey == sHotkeyCfg::controllerNone || !controller)
		return _("----");
	return controller->get_button_name(hotkey);
}

// the first physical controller of player 1, if one is set up
bool HotkeySettings::PickController()
{
	const auto player1 = InputManager::instance().get_controller(0);
	if (player1.use_count() <= 1)
		return false;
	const auto& devices = player1->get_controllers();
	if (devices.empty())
		return false;
	m_controller = devices.front();
	return true;
}

void HotkeySettings::ShowBinding(wxButton* button, Device device)
{
	const sHotkeyCfg& hotkey = HotkeyOf(button);
	button->SetLabelText(device == Device::Keyboard ? KeyboardText(hotkey.keyboard) : ControllerText(hotkey.controller));
}

void HotkeySettings::BeginCapture(wxButton* button, Device device)
{
	if (button == m_capturing)
		return;
	if (m_capturing)
		EndCapture();

	if (device == Device::Controller)
	{
		m_controllerPoll->Stop();
		if (!PickController())
			return;
		m_controllerPoll->Start(25);
	}
	else
	{
		button->Bind(wxEVT_KEY_UP, &HotkeySettings::OnCaptureKeyUp, this);
	}
	button->SetLabelText(wxEmptyString);
	m_capturing = button;
	m_capturingDevice = device;
}

void HotkeySettings::EndCapture()
{
	if (!m_capturing)
		return;
	if (m_capturingDevice == Device::Keyboard)
		m_capturing->Unbind(wxEVT_KEY_UP, &HotkeySettings::OnCaptureKeyUp, this);
	else
		m_controllerPoll->Stop();
	ShowBinding(m_capturing, m_capturingDevice);
	m_capturing = nullptr;
}

void HotkeySettings::ClearBinding(wxButton* button, Device device)
{
	sHotkeyCfg& hotkey = HotkeyOf(button);
	if (device == Device::Keyboard)
	{
		if (hotkey.keyboard.raw == sHotkeyCfg::keyboardNone)
			return;
		s_keyActions.erase(hotkey.keyboard.raw);
		hotkey.keyboard.raw = sHotkeyCfg::keyboardNone;
	}
	else
	{
		if (hotkey.controller == sHotkeyCfg::controllerNone)
			return;
		s_buttonActions.erase(hotkey.controller);
		hotkey.controller = sHotkeyCfg::controllerNone;
	}
	m_dirty = true;
	ShowBinding(button, device);
}

void HotkeySettings::OnCaptureKeyUp(wxKeyEvent& event)
{
	if (!m_capturing)
		return;
	sHotkeyCfg& hotkey = HotkeyOf(m_capturing);
	if (IsBindableKey(event.GetKeyCode()))
	{
		const uKeyboardHotkey pressed = KeyOf(event);
		const uint16 previous = hotkey.keyboard.raw;
		// unchanged, or already used by another hotkey: keep the old binding
		if (pressed.raw != previous && !s_keyActions.contains(pressed.raw))
		{
			hotkey.keyboard = pressed;
			s_keyActions.erase(previous);
			s_keyActions[pressed.raw] = s_cfgHotkeyToFuncMap.at(&hotkey);
			m_dirty = true;
		}
	}
	EndCapture();
}

void HotkeySettings::OnControllerPoll(wxTimerEvent& event)
{
	const auto controller = m_controller.lock();
	if (!controller)
	{
		m_controllerPoll->Stop();
		return;
	}
	const auto buttons = controller->update_state().buttons.GetButtonList();
	if (buttons.empty() || !m_capturing)
		return;

	sHotkeyCfg& hotkey = HotkeyOf(m_capturing);
	const ControllerHotkey_t pressed = buttons.front();
	const ControllerHotkey_t previous = hotkey.controller;
	const bool isModifier = &hotkey == &s_cfgHotkeys.modifiers;
	// not unchanged, not the modifier button, not bound to another hotkey
	const bool accept = pressed != previous && (isModifier || pressed != s_cfgHotkeys.modifiers.controller) && !s_buttonActions.contains(pressed);
	if (accept)
	{
		hotkey.controller = pressed;
		m_dirty = true;
		// the modifier only gates the others and has no action
		if (!isModifier)
		{
			s_buttonActions.erase(previous);
			s_buttonActions[pressed] = s_cfgHotkeyToFuncMap.at(&hotkey);
		}
	}
	EndCapture();
}
