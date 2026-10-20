// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxCemuConfig.h"
#include "wxgui/wxgui.h"
#include "wxgui/GeneralSettings2.h"
#include "wxgui/CemuApp.h"
#include "wxgui/wxHelper.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/dialogs/CreateAccount/wxCreateAccountDialog.h"

#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"
#include "config/LaunchSettings.h"
#include "config/ActiveSettings.h"

#include "audio/IAudioAPI.h"
#include "audio/IAudioInputAPI.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/Account/Account.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cemu/ncrypto/ncrypto.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif

#include "util/helpers/helpers.h"
#include "util/helpers/SystemException.h"
#include "util/ScreenSaver/ScreenSaver.h"
#include "resource/embedded/resources.h"

#include <wx/bmpbuttn.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clrpicker.h>
#include <wx/collpane.h>
#include <wx/dirdlg.h>
#include <wx/hyperlink.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/radiobox.h>
#include <wx/slider.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/propgrid/props.h>

#include <optional>
#include <string>
#include <vector>

wxDEFINE_EVENT(wxEVT_ACCOUNTLIST_REFRESH, wxCommandEvent);

namespace
{
	// ---- pure helpers (GS_PURE_BEGIN)

	// audio back ends in the order they are offered
	struct AudioApiEntry
	{
		IAudioAPI::AudioAPI api;
		const char* name;
	};
	constexpr AudioApiEntry kAudioApis[] = {
		{ IAudioAPI::DirectSound, "DirectSound" },
		{ IAudioAPI::XAudio27, "XAudio2.7" },
		{ IAudioAPI::XAudio2, "XAudio2" },
		{ IAudioAPI::Cubeb, "Cubeb" },
	};

	std::optional<IAudioAPI::AudioAPI> AudioApiFromName(std::string_view name)
	{
		for (const AudioApiEntry& entry : kAudioApis)
		{
			if (name == entry.name)
				return entry.api;
		}
		return std::nullopt;
	}

	const char* AudioApiName(sint32 api)
	{
		for (const AudioApiEntry& entry : kAudioApis)
		{
			if (api == (sint32)entry.api)
				return entry.name;
		}
		return nullptr;
	}

	// text scale choices are 50% .. 300% in steps of 25
	sint32 ChoiceToTextScale(int choice)
	{
		return choice * 25 + 50;
	}

	uint32 TextScaleToChoice(sint32 scale)
	{
		return (uint32)((scale - 50) / 25);
	}

	// window positions/sizes are remembered unless stored as {-1,-1}; {0,0} asks the windows to store theirs
	Vector2i RememberMarker(bool remember)
	{
		return remember ? Vector2i{ 0, 0 } : Vector2i{ -1, -1 };
	}

	bool IsRemembered(const Vector2i& stored)
	{
		return stored != Vector2i{ -1, -1 };
	}

	// "yyyy-mm-dd" -> its non-empty parts
	std::vector<std::string> SplitDate(const std::string& text)
	{
		std::vector<std::string> parts;
		std::string current;
		for (const char c : text)
		{
			if (c == '-')
			{
				if (!current.empty())
					parts.emplace_back(std::move(current));
				current.clear();
			}
			else
				current += c;
		}
		if (!current.empty())
			parts.emplace_back(std::move(current));
		return parts;
	}

	// the first entry is always shown, other unknown ("NN") slots are left out
	bool IsListedCountry(int index, const char* code)
	{
		return code && (index == 0 || std::string_view(code) != "NN");
	}

	// OpenGL only knows off/on
	int OpenGLVsyncChoice(int previous)
	{
		return previous == 0 ? 0 : 1;
	}

	// a device choice's entry 0 is "Disabled"
	bool IsRealDeviceChoice(int selection)
	{
		return selection != wxNOT_FOUND && selection != 0;
	}

	constexpr size_t kMaxListedMissingFiles = 11;

	// ---- GS_PURE_END

	template<typename TPtr>
	class AudioDeviceData : public wxClientData
	{
	public:
		explicit AudioDeviceData(TPtr description) : m_description(std::move(description)) {}
		const TPtr& Get() const { return m_description; }

	private:
		TPtr m_description;
	};
	using OutputDeviceData = AudioDeviceData<IAudioAPI::DeviceDescriptionPtr>;
	using InputDeviceData = AudioDeviceData<IAudioInputAPI::DeviceDescriptionPtr>;

	template<typename TInfo>
	class RendererDeviceData : public wxClientData
	{
	public:
		explicit RendererDeviceData(const TInfo& info) : m_info(info) {}
		const TInfo& Get() const { return m_info; }

	private:
		TInfo m_info;
	};

	class AccountChoiceData : public wxClientData
	{
	public:
		explicit AccountChoiceData(const Account& account) : m_account(account) {}
		Account& Get() { return m_account; }

	private:
		Account m_account;
	};

	// account grid property names
	const wxString kPropPersistentId("PersistentId");
	const wxString kPropMiiName("MiiName");
	const wxString kPropBirthday("Birthday");
	const wxString kPropGender("Gender");
	const wxString kPropEmail("Email");
	const wxString kPropCountry("Country");

	constexpr int kMaxAccounts = 0xC;
	constexpr int kAxFramesPerGroup = 4;

	wxFlexGridSizer* NewGrid(int cols, int rows = 0, int vgap = 0)
	{
		auto* grid = new wxFlexGridSizer(rows, cols, vgap, 0);
		grid->SetFlexibleDirection(wxBOTH);
		grid->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);
		return grid;
	}

	wxStaticText* AddLabel(wxWindow* parent, wxSizer* sizer, const wxString& text)
	{
		auto* label = new wxStaticText(parent, wxID_ANY, text);
		sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		return label;
	}

	wxCheckBox* NewCheckBox(wxWindow* parent, const wxString& label, const wxString& tip)
	{
		auto* box = new wxCheckBox(parent, wxID_ANY, label);
		box->SetToolTip(tip);
		return box;
	}

	wxStaticBoxSizer* NewSection(wxWindow* parent, const wxString& title)
	{
		return new wxStaticBoxSizer(new wxStaticBox(parent, wxID_ANY, title), wxVERTICAL);
	}

	// keeps a slider's value label in sync; the slider also takes the event's value (SendSliderEvent relies on this)
	void BindSliderLabel(wxSlider* slider, wxStaticText* label, int factor, const char* unit)
	{
		slider->Bind(wxEVT_SLIDER, [slider, label, factor, unit](wxCommandEvent& event) {
			slider->SetValue(event.GetInt());
			label->SetLabel(wxString::Format("%d%s", event.GetInt() * factor, unit));
			event.Skip();
		});
	}

	wxBitmap SmallIcon(const wxBitmap& bitmap)
	{
		return wxBitmap(bitmap.ConvertToImage().Scale(16, 16));
	}

	template<typename TData>
	void SelectStoredDevice(wxChoice* choice, const std::wstring& identifier)
	{
		if (identifier.empty() || !choice->HasClientObjectData())
		{
			choice->SetSelection(0);
			return;
		}
		for (unsigned int i = 0; i < choice->GetCount(); ++i)
		{
			const auto* data = static_cast<TData*>(choice->GetClientObject(i));
			if (data && data->Get()->GetIdentifier() == identifier)
			{
				choice->SetSelection(i);
				return;
			}
		}
	}

	template<typename TData>
	std::wstring SelectedDeviceIdentifier(wxChoice* choice)
	{
		const int selection = choice->GetSelection();
		if (!IsRealDeviceChoice(selection) || !choice->HasClientObjectData())
			return {};
		const auto* data = static_cast<TData*>(choice->GetClientObject(selection));
		return data ? data->Get()->GetIdentifier() : std::wstring();
	}

	template<typename TData>
	const TData* SelectedData(wxChoice* choice)
	{
		const int selection = choice->GetSelection();
		if (selection == wxNOT_FOUND || !choice->HasClientObjectData())
			return nullptr;
		return static_cast<TData*>(choice->GetClientObject(selection));
	}

	void TellParent(wxWindow* parent)
	{
		wxCommandEvent refresh(wxEVT_ACCOUNTLIST_REFRESH);
		parent->ProcessWindowEvent(refresh);
	}

	void ShowRestartAndQuit(wxWindow* dialog)
	{
		wxMessageBox(_("PixelU needs to be restarted for the changes to take effect."), _("Information"), wxOK | wxCENTRE | wxICON_INFORMATION, dialog);
		// closes the settings, then the main window
		wxCloseEvent close(wxEVT_CLOSE_WINDOW);
		wxPostEvent(dialog, close);
		wxPostEvent(dialog->GetParent(), close);
	}
}

// ---------------------------------------------------------------- construction

GeneralSettings2::GeneralSettings2(wxWindow* parent, bool game_launched)
	: wxDialog(parent, wxID_ANY, _("General settings"), wxDefaultPosition, wxDefaultSize, wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER),
	  m_game_launched(game_launched)
{
	SetIcon(wxICON(X_SETTINGS));

	auto* notebook = new wxNotebook(this, wxID_ANY);
	notebook->AddPage(AddGeneralPage(notebook), _("General"));
	notebook->AddPage(AddGraphicsPage(notebook), _("Graphics"));
	notebook->AddPage(AddAudioPage(notebook), _("Audio"));
	notebook->AddPage(AddOverlayPage(notebook), _("Overlay"));
	notebook->AddPage(AddAccountPage(notebook), _("Account"));
	notebook->AddPage(AddDebugPage(notebook), _("Debug"));

	Bind(wxEVT_CLOSE_WINDOW, &GeneralSettings2::OnClose, this);

	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(notebook, 1, wxEXPAND | wxALL, 5);
	SetSizerAndFit(layout);
	Layout();
	Centre(wxBOTH);

	UpdateOnlineAccounts();
	UpdateAudioDeviceList();
	ApplyConfig();
	HandleGraphicsApiSelection();
	DisableSettings(game_launched);
}

GeneralSettings2::~GeneralSettings2()
{
	Unbind(wxEVT_CLOSE_WINDOW, &GeneralSettings2::OnClose, this);
}

void GeneralSettings2::OnClose(wxCloseEvent& event)
{
	StoreConfig();
	if (m_has_account_change)
		TellParent(GetParent());
	event.Skip();
}

wxPanel* GeneralSettings2::AddGeneralPage(wxNotebook* notebook)
{
	auto* page = new wxPanel(notebook);
	auto* column = new wxBoxSizer(wxVERTICAL);

	// --- Interface
	{
		auto* section = NewSection(page, _("Interface"));
		wxWindow* box = section->GetStaticBox();

		auto* languageRow = NewGrid(2);
		AddLabel(box, languageRow, _("Language"));
		const wxString defaultLanguage[] = { _("Default") };
		m_language = new wxChoice(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, 1, defaultLanguage);
		m_language->SetSelection(0);
		m_language->SetToolTip(_("Changes the interface language of PixelU\nAvailable languages are stored in the translation directory\nA restart will be required after changing the language"));
		for (const wxLanguageInfo* language : wxGetApp().GetLanguages())
			m_language->Append(language->DescriptionNative);
		languageRow->Add(m_language, 0, wxALL | wxEXPAND, 5);
		section->Add(languageRow, 1, wxEXPAND, 5);

#if BOOST_OS_WINDOWS
		auto* themeRow = NewGrid(2);
		AddLabel(box, themeRow, _("Theme"));
		m_msw_theme = new wxChoice(box, wxID_ANY);
		m_msw_theme->SetToolTip(_("Changes the Windows theme used by PixelU\nThis only works on Windows 10 and later\nA restart will be required for any changes to take effect"));
		m_msw_theme->AppendString(_("Follow Windows theme"));
		m_msw_theme->AppendString(_("Light Theme"));
		m_msw_theme->AppendString(_("Dark Theme"));
		m_msw_theme->SetSelection(0);
		themeRow->Add(m_msw_theme, 0, wxALL, 5);
		section->Add(themeRow, 0, wxEXPAND, 5);
#endif

		// two check boxes per row, then a spacer column
		auto* options = NewGrid(3);
		int inRow = 0;
		auto place = [&](wxCheckBox* check, bool topRow, bool countInRow = true) {
			options->Add(check, 0, wxALIGN_CENTER_VERTICAL | (topRow ? wxALL : (wxLEFT | wxRIGHT | wxBOTTOM)), 5);
			if (countInRow && ++inRow == 2)
			{
				options->AddSpacer(10);
				inRow = 0;
			}
			return check;
		};

		m_save_window_position_size = place(NewCheckBox(box, _("Remember main window position"), _("Restores the last known window position and size when starting PixelU")), true);
		m_save_padwindow_position_size = place(NewCheckBox(box, _("Remember pad window position"), _("Restores the last known pad window position and size when opening it")), true);
		m_discord_presence = place(NewCheckBox(box, _("Discord Presence"), _("Enables the Discord Rich Presence feature\nYou will also need to enable it in the Discord settings itself!")), false);
#ifndef ENABLE_DISCORD_RPC
		m_discord_presence->Disable();
#endif
		m_fullscreen_menubar = place(NewCheckBox(box, _("Fullscreen menu bar"), _("Displays the menu bar when PixelU is running in fullscreen mode and the mouse cursor is moved to the top")), false);
		m_save_screenshot = place(NewCheckBox(box, _("Save screenshot"), _("Pressing the screenshot key will save a screenshot directly to the screenshots folder instead of to the clipboard")), false);
		m_disable_screensaver = place(NewCheckBox(box, _("Disable screen saver"), _("Prevents the system from activating the screen saver or going to sleep while running a game.")), false);
#if BOOST_OS_LINUX && defined(ENABLE_FERAL_GAMEMODE)
		m_feral_gamemode = place(NewCheckBox(box, _("Enable Feral GameMode"), _("Use FeralInteractive GameMode if installed.")), false);
#endif
#if BOOST_OS_MACOS
		m_disable_screensaver->Enable(false); // the screen saver inhibit crashes on macOS
#endif
		m_play_boot_sound = place(NewCheckBox(box, _("Enable intro sound"), _("Play bootSound file while compiling shaders/pipelines.")), false);
		m_auto_update = place(NewCheckBox(box, _("Automatically check for updates"), _("Automatically checks for new PixelU versions on startup")), false);
		m_receive_untested_releases = place(NewCheckBox(box, _("Receive untested updates"), _("When checking for updates, include brand new and untested releases. These may contain bugs!")), false, false);
#if BOOST_OS_LINUX
		if (!std::getenv("APPIMAGE"))
			m_auto_update->Disable(); // only AppImage builds can update themselves
#elif BOOST_OS_BSD
		m_auto_update->Disable(); // built from source
#endif
		section->Add(options, 0, wxEXPAND, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);
	}

	// --- MLC
	{
		auto* section = NewSection(page, _("Custom MLC path"));
		wxWindow* box = section->GetStaticBox();
		section->Add(new wxStaticText(box, wxID_ANY, _("You can configure a custom path for the emulated internal Wii U storage (MLC).\nThis is where PixelU stores saves, accounts and other Wii U system files.")), 0, wxALL, 5);

		auto* pathRow = new wxBoxSizer(wxHORIZONTAL);
		m_mlc_path = new wxTextCtrl(box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
		m_mlc_path->SetMinSize(wxSize(150, -1));
		m_mlc_path->SetToolTip(_("The mlc directory contains your save games and installed game update/dlc data"));
		pathRow->Add(m_mlc_path, 1, wxALL | wxEXPAND, 5);

		// a path given on the command line wins and cannot be changed here
		const bool fixedByLaunch = LaunchSettings::GetMLCPath().has_value();
		auto* change = new wxButton(box, wxID_ANY, _("Change"));
		change->Bind(wxEVT_BUTTON, &GeneralSettings2::OnMLCPathSelect, this);
		change->Enable(!fixedByLaunch);
		pathRow->Add(change, 0, wxALL, 5);

		auto* clear = new wxButton(box, wxID_ANY, _("Clear custom path"));
		clear->Bind(wxEVT_BUTTON, &GeneralSettings2::OnMLCPathClear, this);
		clear->Enable(!fixedByLaunch && ActiveSettings::IsCustomMlcPath());
		pathRow->Add(clear, 0, wxALL, 5);

		section->Add(pathRow, 0, wxEXPAND, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);
	}

	// --- Game paths
	{
		auto* section = NewSection(page, _("Game Paths"));
		wxWindow* box = section->GetStaticBox();

		m_game_paths = new wxListBox(box, wxID_ANY);
		m_game_paths->SetMinSize(wxSize(150, 70));
		m_game_paths->SetToolTip(_("Add the root directory of your game(s). It will scan all directories in it for games"));
		section->Add(m_game_paths, 1, wxALL | wxEXPAND, 5);

		auto* buttons = NewGrid(2);
		auto* add = new wxButton(box, wxID_ANY, _("Add"));
		add->SetToolTip(_("Adds a game path to scan for games displayed in the game list\nIf you have unpacked games, make sure to select the root folder of a game"));
		add->Bind(wxEVT_BUTTON, &GeneralSettings2::OnAddPathClicked, this);
		buttons->Add(add, 0, wxALL, 5);
		auto* remove = new wxButton(box, wxID_ANY, _("Remove"));
		remove->SetToolTip(_("Removes the currently selected game path from the game list"));
		remove->Bind(wxEVT_BUTTON, &GeneralSettings2::OnRemovePathClicked, this);
		buttons->Add(remove, 0, wxALL, 5);
		section->Add(buttons, 0, wxEXPAND, 5);

		column->Add(section, 1, wxEXPAND | wxALL, 5);
	}

	page->SetSizerAndFit(column);
	return page;
}

wxPanel* GeneralSettings2::AddGraphicsPage(wxNotebook* notebook)
{
	auto* page = new wxPanel(notebook);
	auto* column = new wxBoxSizer(wxVERTICAL);

	// --- API, device, vsync
	{
		auto* section = NewSection(page, _("General"));
		wxWindow* box = section->GetStaticBox();
		auto* grid = NewGrid(2);

		wxArrayString apiNames;
#ifdef ENABLE_OPENGL
		apiNames.Add("OpenGL");
		m_api_map.push_back(GraphicAPI::kOpenGL);
#endif
#ifdef ENABLE_VULKAN
		if (g_vulkan_available)
		{
			apiNames.Add("Vulkan");
			m_api_map.push_back(GraphicAPI::kVulkan);
		}
#endif
		wxASSERT(!m_api_map.empty());

		AddLabel(box, grid, _("Graphics API"));
		m_graphic_api = new wxChoice(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, apiNames);
		m_graphic_api->SetSelection(0);
		if (apiNames.size() > 1)
			m_graphic_api->SetToolTip(_("Select one of the available graphic back ends"));
		m_graphic_api->Bind(wxEVT_CHOICE, &GeneralSettings2::OnGraphicAPISelected, this);
		grid->Add(m_graphic_api, 0, wxALL, 5);

		// filled by HandleGraphicsApiSelection
		AddLabel(box, grid, _("Graphics Device"));
		m_graphic_device = new wxChoice(box, wxID_ANY, wxDefaultPosition, { 230, -1 });
		m_graphic_device->SetToolTip(_("Select the used graphic device"));
		grid->Add(m_graphic_device, 0, wxALL, 5);

		AddLabel(box, grid, _("VSync"));
		m_vsync = new wxChoice(box, wxID_ANY, wxDefaultPosition, { 230, -1 });
		m_vsync->SetToolTip(_("Controls the vsync state"));
		m_vsync->Bind(wxEVT_CHOICE, [](wxCommandEvent& event) { GetConfig().vsync = event.GetSelection(); });
		grid->Add(m_vsync, 0, wxALL, 5);
		section->Add(grid, 0, wxEXPAND, 5);

		auto* misc = new wxFlexGridSizer(0, 2, 0, 0);
		m_async_compile = NewCheckBox(box, _("Async shader compile"), _("Enables async shader and pipeline compilation. Reduces stutter at the cost of objects not rendering for a short time.\nVulkan only"));
		misc->Add(m_async_compile, 0, wxALL, 5);
		m_gx2drawdone_sync = NewCheckBox(box, _("Full sync at GX2DrawDone()"), _("If synchronization is requested by the game, the emulated CPU will wait for the GPU to finish all operations.\nThis is more accurate behavior, but may cause lower performance"));
		misc->Add(m_gx2drawdone_sync, 0, wxALL, 5);
		section->Add(misc, 1, wxEXPAND, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);
	}

	// --- Gamma (spin changes apply right away)
	{
		auto* section = NewSection(page, _("Gamma settings"));
		wxWindow* box = section->GetStaticBox();
		auto* grid = NewGrid(2);
		auto newGammaSpin = [box]() {
			return new wxSpinCtrlDouble(box, wxID_ANY, "2.2f", wxDefaultPosition, { 230, -1 }, wxSP_ARROW_KEYS, 0.1f, 4.0f, 2.2f, 0.1f);
		};

		const wxString targetTip = _("The display gamma to reproduce\nIf you are unsure, set this to 2.2");
		AddLabel(box, grid, _("Target Gamma"))->SetToolTip(targetTip);
		m_overrideGammaValue = newGammaSpin();
		m_overrideGammaValue->SetToolTip(targetTip);
		m_overrideGammaValue->Bind(wxEVT_SPINCTRLDOUBLE, [](wxSpinDoubleEvent& event) { GetConfig().overrideGammaValue = event.GetValue(); });
		grid->Add(m_overrideGammaValue, 0, wxALL, 5);

		const wxString displayTip = _("The gamma of your monitor\nIf you are unsure, set this to 2.2");
		AddLabel(box, grid, _("Display Gamma"))->SetToolTip(displayTip);
		auto* displayRow = new wxBoxSizer(wxHORIZONTAL);
		m_userDisplayGamma = newGammaSpin();
		m_userDisplayGamma->SetToolTip(displayTip);
		m_userDisplayGamma->Bind(wxEVT_SPINCTRLDOUBLE, [](wxSpinDoubleEvent& event) { GetConfig().userDisplayGamma = event.GetValue(); });
		m_userDisplayisSRGB = NewCheckBox(box, "sRGB", _("Select this if PixelU is being displayed using a piecewise sRGB gamma curve.\n"
														  "This is typically not the case so you can probably leave this unchecked.\n"
														  "Exceptions include HDR displays (with HDR enabled), calibrated SDR displays with Windows 11's Auto Color Management enabled, "
														  "or when using a display profile with a VCGT tag that targets piecewise sRGB.\n"
														  "When this box is selected PixelU will compensate for the piecewise curve to approximate the pure gamma curve of a TV.\n"
														  "Colors will be more accurate, especially in dark scenes, but this may result in banding or crushed shadows, "
														  "so it is best if you display PixelU with pure gamma and do not use this setting."));
		m_userDisplayisSRGB->Bind(wxEVT_CHECKBOX, &GeneralSettings2::OnUserDisplaySRGBSelected, this);
		displayRow->Add(m_userDisplayGamma, 0, wxALL, 5);
		displayRow->Add(m_userDisplayisSRGB, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		grid->Add(displayRow);

		AddLabel(box, grid, _("Override Gamma"));
		m_overrideGamma = new wxCheckBox(box, wxID_ANY, "", wxDefaultPosition, { 230, -1 });
		m_overrideGamma->SetToolTip(_("Ignore title's gamma preference"));
		m_overrideGamma->Bind(wxEVT_CHECKBOX, [](wxCommandEvent& event) { GetConfig().overrideAppGammaPreference = event.IsChecked(); });
		grid->Add(m_overrideGamma, 0, wxALL, 5);

		section->Add(grid, 0, wxEXPAND, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);
	}

	// --- scaling (apply right away)
	auto addRadio = [&](const wxString& title, const wxArrayString& choices, const wxString& tip, auto store) {
		auto* radio = new wxRadioBox(page, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, choices, 5, wxRA_SPECIFY_COLS);
		radio->SetToolTip(tip);
		radio->Bind(wxEVT_RADIOBOX, [store](wxCommandEvent& event) { store(event.GetSelection()); });
		column->Add(radio, 0, wxALL | wxEXPAND, 5);
		return radio;
	};
	wxArrayString filters;
	filters.Add(_("Bilinear"));
	filters.Add(_("Bicubic"));
	filters.Add(_("Hermite"));
	filters.Add(_("Nearest Neighbor"));
	m_upscale_filter = addRadio(_("Upscale filter"), filters, _("Upscaling filters are used when the game resolution is smaller than the window size"),
		[](int choice) { GetConfig().upscale_filter = choice; });
	m_downscale_filter = addRadio(_("Downscale filter"), filters, _("Downscaling filters are used when the game resolution is bigger than the window size"),
		[](int choice) { GetConfig().downscale_filter = choice; });
	wxArrayString scaling;
	scaling.Add(_("Keep aspect ratio"));
	scaling.Add(_("Stretch"));
	m_fullscreen_scaling = addRadio(_("Fullscreen scaling"), scaling, _("Controls the output aspect ratio when it doesn't match the ratio of the game"),
		[](int choice) { GetConfig().fullscreen_scaling = choice; });

	page->SetSizerAndFit(column);
	return page;
}

wxPanel* GeneralSettings2::AddAudioPage(wxNotebook* notebook)
{
	auto* page = new wxPanel(notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	auto* column = new wxBoxSizer(wxVERTICAL);

	// --- API and latency
	{
		auto* section = NewSection(page, _("General"));
		wxWindow* box = section->GetStaticBox();
		auto* grid = NewGrid(3);

		AddLabel(box, grid, _("API"));
		m_audio_api = new wxChoice(box, wxID_ANY);
		for (const AudioApiEntry& entry : kAudioApis)
		{
			if (IAudioAPI::IsAudioAPIAvailable(entry.api))
				m_audio_api->Append(entry.name);
		}
		m_audio_api->SetSelection(0);
		m_audio_api->SetToolTip(_("Select one of the available audio back ends"));
		m_audio_api->Bind(wxEVT_CHOICE, &GeneralSettings2::OnAudioAPISelected, this);
		grid->Add(m_audio_api, 0, wxALL, 5);
		grid->AddSpacer(0);

		AddLabel(box, grid, _("Latency"));
		m_audio_latency = new wxSlider(box, wxID_ANY, 2, 0, IAudioAPI::kBlockCount - 1, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
		m_audio_latency->SetToolTip(_("Controls the amount of buffered audio data\nHigher values will create a delay in audio playback, but may avoid audio problems when emulation is too slow"));
		grid->Add(m_audio_latency, 0, wxEXPAND | wxALL, 5);
		auto* latencyText = new wxStaticText(box, wxID_ANY, "24ms");
		grid->Add(latencyText, 0, wxALIGN_CENTER_VERTICAL | wxALL | wxALIGN_RIGHT, 5);
		BindSliderLabel(m_audio_latency, latencyText, 12, "ms"); // one block is 12ms
		m_audio_latency->Bind(wxEVT_SLIDER, &GeneralSettings2::OnAudioLatencyChanged, this);

		section->Add(grid, 1, wxEXPAND, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);
	}

	// --- one section per output/input: device, channels (not for the portal), volume
	struct AudioSection
	{
		wxString title;
		wxString deviceTip;
		std::vector<wxString> channels;
		int defaultChannel;
		wxChoice** device;
		wxChoice** channelChoice;
		wxSlider** volume;
	};
	const AudioSection sections[] = {
		{ _("TV"), _("Select the active audio output device for Wii U TV"), { _("Mono"), _("Stereo"), _("Surround") }, 1, &m_tv_device, &m_tv_channels, &m_tv_volume },
		{ _("Gamepad"), _("Select the active audio output device for Wii U GamePad"), { _("Stereo") }, 0, &m_pad_device, &m_pad_channels, &m_pad_volume },
		{ _("Microphone (Experimental)"), _("Select the active audio input device for Wii U GamePad"), { _("Mono") }, 0, &m_input_device, &m_input_channels, &m_input_volume },
		{ _("Trap Team Portal"), _("Select the active audio output device for the Trap Team Portal"), {}, 0, &m_portal_device, nullptr, &m_portal_volume },
	};
	for (const AudioSection& spec : sections)
	{
		auto* section = NewSection(page, spec.title);
		wxWindow* box = section->GetStaticBox();
		auto* grid = NewGrid(3);

		AddLabel(box, grid, _("Device"));
		wxChoice* device = new wxChoice(box, wxID_ANY);
		device->SetMinSize(wxSize(300, -1));
		device->SetToolTip(spec.deviceTip);
		device->Bind(wxEVT_CHOICE, &GeneralSettings2::OnAudioDeviceSelected, this);
		grid->Add(device, 0, wxEXPAND | wxALL, 5);
		grid->AddSpacer(0);
		*spec.device = device;

		if (spec.channelChoice)
		{
			AddLabel(box, grid, _("Channels"));
			wxChoice* channels = new wxChoice(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, (int)spec.channels.size(), spec.channels.data());
			channels->SetSelection(spec.defaultChannel);
			channels->Bind(wxEVT_CHOICE, &GeneralSettings2::OnAudioChannelsSelected, this);
			grid->Add(channels, 0, wxEXPAND | wxALL, 5);
			grid->AddSpacer(0);
			*spec.channelChoice = channels;
		}

		AddLabel(box, grid, _("Volume"));
		wxSlider* volume = new wxSlider(box, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
		grid->Add(volume, 0, wxEXPAND | wxALL, 5);
		auto* volumeText = new wxStaticText(box, wxID_ANY, "100%");
		grid->Add(volumeText, 0, wxALIGN_CENTER_VERTICAL | wxALL | wxALIGN_RIGHT, 5);
		BindSliderLabel(volume, volumeText, 1, "%");
		volume->Bind(wxEVT_SLIDER, &GeneralSettings2::OnVolumeChanged, this);
		*spec.volume = volume;

		section->Add(grid, 1, wxEXPAND, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);
	}

	page->SetSizerAndFit(column);
	return page;
}

wxPanel* GeneralSettings2::AddOverlayPage(wxNotebook* notebook)
{
	auto* page = new wxPanel(notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	auto* column = new wxBoxSizer(wxVERTICAL);

	wxArrayString positions;
	for (const wxString& name : { _("Disabled"), _("Top left"), _("Top center"), _("Top right"), _("Bottom left"), _("Bottom center"), _("Bottom right") })
		positions.Add(name);
	wxArrayString scales;
	for (int choice = 0; ChoiceToTextScale(choice) <= 300; ++choice)
		scales.Add(wxString::Format("%d%%", ChoiceToTextScale(choice)));

	// position, colour and scale in one row
	auto addPlacementRow = [&](wxWindow* box, wxSizer* section, const wxString& positionTip, const wxString& colourTip, const wxString& scaleTip,
							   wxChoice*& position, wxColourPickerCtrl*& colour, wxChoice*& scale) {
		auto* row = NewGrid(0, 1);
		row->Add(new wxStaticText(box, wxID_ANY, _("Position")), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
		position = new wxChoice(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, positions);
		position->SetSelection(0);
		position->SetToolTip(positionTip);
		row->Add(position, 0, wxALL, 5);
		row->AddSpacer(25);
		row->Add(new wxStaticText(box, wxID_ANY, _("Text Color")), 0, wxALL | wxALIGN_CENTRE_VERTICAL, 5);
		colour = new wxColourPickerCtrl(box, wxID_ANY, *wxWHITE, wxDefaultPosition, wxDefaultSize, wxCLRP_SHOW_ALPHA);
		colour->SetToolTip(colourTip);
		row->Add(colour, 0, wxEXPAND | wxALL, 5);
		row->AddSpacer(25);
		row->Add(new wxStaticText(box, wxID_ANY, _("Scale")), 0, wxALL | wxALIGN_CENTRE_VERTICAL, 5);
		scale = new wxChoice(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, scales);
		scale->SetToolTip(scaleTip);
		row->Add(scale, 0, wxEXPAND | wxALL, 5);
		section->Add(row, 0, wxEXPAND, 5);
	};
	auto addCheck = [](wxWindow* box, wxSizer* grid, const wxString& label, const wxString& tip) {
		wxCheckBox* check = NewCheckBox(box, label, tip);
		grid->Add(check, 0, wxALL, 5);
		return check;
	};

	// --- Overlay
	{
		auto* section = NewSection(page, _("Overlay"));
		wxWindow* box = section->GetStaticBox();
		addPlacementRow(box, section, _("Controls the overlay which displays technical information while playing"), _("Sets the text color of the overlay"),
			_("Sets the scale of the overlay text"), m_overlay_position, m_overlay_font_color, m_overlay_scale);

		auto* checks = new wxFlexGridSizer(0, 4, 2, 0);
		m_overlay_fps = addCheck(box, checks, _("FPS"), _("The number of frames per second. Average over last 5 seconds"));
		m_overlay_drawcalls = addCheck(box, checks, _("Draw calls per frame"), _("The number of draw calls per frame. Average over last 5 seconds"));
		m_overlay_cpu = addCheck(box, checks, _("CPU usage"), _("CPU usage of PixelU in percent"));
		m_overlay_cpu_per_core = addCheck(box, checks, _("CPU per core usage"), _("Total cpu usage in percent for each core"));
		m_overlay_ram = addCheck(box, checks, _("RAM usage"), _("PixelU RAM usage in MB"));
		m_overlay_vram = addCheck(box, checks, _("VRAM usage"), _("The VRAM usage of PixelU in MB"));
#if BOOST_OS_WINDOWS
		// the VRAM query needs Windows 8.1 (6.3); the real version comes from ntdll since the manifest may hide it
		using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
		const auto rtlGetVersion = (RtlGetVersionFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
		RTL_OSVERSIONINFOW version{};
		const bool vramQuerySupported = rtlGetVersion && rtlGetVersion(&version) == 0 &&
										(version.dwMajorVersion > 6 || (version.dwMajorVersion == 6 && version.dwMinorVersion >= 3));
		if (!vramQuerySupported)
		{
			m_overlay_vram->SetToolTip(_("This option requires Win8.1+"));
			m_overlay_vram->Disable();
		}
#endif
		m_overlay_debug = addCheck(box, checks, _("Debug"), _("Displays internal debug information (Vulkan only)"));
		section->Add(checks, 0, wxEXPAND, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);
	}

	// --- Notifications
	{
		auto* section = NewSection(page, _("Notifications"));
		wxWindow* box = section->GetStaticBox();
		addPlacementRow(box, section, _("Controls the notification position while playing"), _("Sets the text color of notifications"),
			_("Sets the scale of the notification text"), m_notification_position, m_notification_font_color, m_notification_scale);

		auto* checks = new wxFlexGridSizer(1, 0, 2, 0);
		m_controller_profile_name = addCheck(box, checks, _("Controller profiles"), _("Displays the active controller profile when starting a game"));
		m_controller_low_battery = addCheck(box, checks, _("Low battery"), _("Shows a notification when a low controller battery has been detected"));
		m_shader_compiling = addCheck(box, checks, _("Shader compiler"), _("Shows a notification after shaders have been compiled"));
		m_friends_data = addCheck(box, checks, _("Friend list"), _("Shows friend list related data if online"));
		section->Add(checks, 0, wxEXPAND, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);
	}

	page->SetSizerAndFit(column);
	return page;
}

wxPanel* GeneralSettings2::AddAccountPage(wxNotebook* notebook)
{
	auto* page = new wxPanel(notebook);
	auto* column = new wxBoxSizer(wxVERTICAL);
	// accounts are fixed while a game runs
	const bool titleRunning = CafeSystem::IsTitleRunning();

	// --- active account
	{
		auto* section = NewSection(page, _("Account settings"));
		wxWindow* box = section->GetStaticBox();
		auto* grid = NewGrid(4);
		grid->AddGrowableCol(1, 1);
		grid->AddGrowableCol(2, 0);
		grid->AddGrowableCol(3, 0);

		grid->Add(new wxStaticText(box, wxID_ANY, _("Active account")), 1, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		m_active_account = new wxChoice(box, wxID_ANY);
		m_active_account->SetMinSize({ 250, -1 });
		m_active_account->Bind(wxEVT_CHOICE, &GeneralSettings2::OnActiveAccountChanged, this);
		grid->Add(m_active_account, 0, wxEXPAND | wxALL, 5);

		m_create_account = new wxButton(box, wxID_ANY, _("Create"));
		m_create_account->Bind(wxEVT_BUTTON, &GeneralSettings2::OnAccountCreate, this);
		grid->Add(m_create_account, 0, wxEXPAND | wxALL | wxALIGN_RIGHT, 5);
		m_delete_account = new wxButton(box, wxID_ANY, _("Delete"));
		m_delete_account->Bind(wxEVT_BUTTON, &GeneralSettings2::OnAccountDelete, this);
		grid->Add(m_delete_account, 0, wxEXPAND | wxALL | wxALIGN_RIGHT, 5);

		section->Add(grid, 1, wxEXPAND, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);

		if (titleRunning)
		{
			m_active_account->Enable(false);
			m_create_account->Enable(false);
			m_delete_account->Enable(false);
		}
	}

	// --- network service (index = NetworkService)
	{
		wxArrayString services;
		services.Add(_("Offline"));
		services.Add(_("Nintendo"));
		services.Add(_("Pretendo"));
		services.Add(_("Custom"));
		m_active_service = new wxRadioBox(page, wxID_ANY, _("Network Service"), wxDefaultPosition, wxDefaultSize, services, 4, wxRA_SPECIFY_COLS);
		m_active_service->SetItemToolTip(0, _("Online functionality disabled for this account"));
		m_active_service->SetItemToolTip(1, _("Connect to the official Nintendo Network Service"));
		m_active_service->SetItemToolTip(2, _("Connect to the Pretendo Network Service"));
		m_active_service->SetItemToolTip(3, _("Connect to a custom Network Service (configured via network_services.xml)"));
		if (!NetworkConfig::XMLExists())
			m_active_service->Enable(3, false);
		m_active_service->Bind(wxEVT_RADIOBOX, &GeneralSettings2::OnAccountServiceChanged, this);
		column->Add(m_active_service, 0, wxEXPAND | wxALL, 5);
		if (titleRunning)
			m_active_service->Enable(false);
	}

	// --- online requirements
	{
		auto* section = NewSection(page, _("Online play requirements"));
		wxWindow* box = section->GetStaticBox();
		auto* row = NewGrid(2);

		m_validate_online = new wxBitmapButton(box, wxID_ANY, SmallIcon(wxBITMAP_PNG_FROM_DATA(PNG_ERROR)));
		m_validate_online->Bind(wxEVT_BUTTON, &GeneralSettings2::OnShowOnlineValidator, this);
		row->Add(m_validate_online, 0, wxEXPAND | wxALL, 5);
		m_online_status = new wxStaticText(box, wxID_ANY, _("No account selected"));
		row->Add(m_online_status, 1, wxALL | wxALIGN_CENTRE_VERTICAL, 5);
		section->Add(row, 1, wxEXPAND, 5);

		section->Add(new wxHyperlinkCtrl(box, wxID_ANY, _("Online play tutorial"), "https://cemu.info/online-guide"), 0, wxALL, 5);
		column->Add(section, 0, wxEXPAND | wxALL, 5);
	}

	// --- details of the selected account (editable)
	{
		m_account_information = new wxCollapsiblePane(page, wxID_ANY, _("Account information"));
		wxWindow* pane = m_account_information->GetPane();
		auto* paneSizer = new wxBoxSizer(wxVERTICAL);

		m_account_grid = new wxPropertyGrid(pane, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxPG_HIDE_MARGIN | wxPG_STATIC_SPLITTER);
		m_account_grid->SetExtraStyle(wxPG_EX_HELP_AS_TOOLTIPS);
		m_account_grid->SetMinSize({ 300, -1 });

		wxPGProperty* persistentId = m_account_grid->Append(new wxStringProperty("PersistentId", kPropPersistentId));
		persistentId->SetHelpString(_("The persistent id is the internal folder name used for your saves"));
		m_account_grid->SetPropertyReadOnly(persistentId);
		m_account_grid->Append(new wxStringProperty(_("Mii name"), kPropMiiName))->SetHelpString(_("The mii name is the profile name"));
		m_account_grid->Append(new wxStringProperty(_("Birthday"), kPropBirthday));

		wxPGChoices genders;
		genders.Add(_("Female"), 0);
		genders.Add(_("Male"), 1);
		m_account_grid->Append(new wxEnumProperty(_("Gender"), kPropGender, genders));
		m_account_grid->Append(new wxStringProperty(_("Email"), kPropEmail));

		wxPGChoices countries;
		for (int i = 0; i < (int)NCrypto::GetCountryCount(); ++i)
		{
			const char* code = NCrypto::GetCountryAsString(i);
			if (IsListedCountry(i, code))
				countries.Add(code, i);
		}
		m_account_grid->Append(new wxEnumProperty(_("Country"), kPropCountry, countries));
		m_account_grid->Bind(wxEVT_PG_CHANGED, &GeneralSettings2::OnAccountSettingsChanged, this);

		paneSizer->Add(m_account_grid, 1, wxEXPAND | wxALL, 5);
		pane->SetSizer(paneSizer);
		paneSizer->SetSizeHints(pane);
		column->Add(m_account_information, 0, wxEXPAND | wxALL, 5);
	}

	page->SetSizerAndFit(column);
	return page;
}

wxPanel* GeneralSettings2::AddDebugPage(wxNotebook* notebook)
{
	auto* page = new wxPanel(notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	auto* column = new wxBoxSizer(wxVERTICAL);
	auto addRow = [&](const wxString& label, wxWindow* control) {
		auto* row = NewGrid(2);
		AddLabel(page, row, label);
		row->Add(control, 0, wxALL | wxEXPAND, 5);
		column->Add(row, 0, wxALL | wxEXPAND, 5);
	};

	// index = CrashDump
	wxArrayString dumpModes;
	dumpModes.Add(_("Disabled"));
#if BOOST_OS_WINDOWS
	dumpModes.Add(_("Lite"));
	dumpModes.Add(_("Full"));
#elif BOOST_OS_UNIX
	dumpModes.Add(_("Enabled"));
#endif
	m_crash_dump = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, dumpModes);
	m_crash_dump->SetSelection(0);
#if BOOST_OS_WINDOWS
	m_crash_dump->SetToolTip(_("Creates a dump when PixelU crashes\nOnly enable when requested by a developer!\nThe Full option will create a very large dump file (includes a full RAM dump of the PixelU process)"));
#elif BOOST_OS_UNIX
	m_crash_dump->SetToolTip(_("Creates a core dump when PixelU crashes\nOnly enable when requested by a developer!"));
#endif
	addRow(_("Crash dump"), m_crash_dump);

	m_gdb_port = new wxSpinCtrl(page, wxID_ANY, "1337", wxDefaultPosition, wxDefaultSize, 0, 1000, 65535);
	m_gdb_port->SetToolTip(_("Changes the port that the GDB stub will use, which you can use by either starting PixelU with the --enable-gdbstub option or by enabling it the Debug tab."));
	addRow(_("GDB Stub port"), m_gdb_port);


	page->SetSizerAndFit(column);
	return page;
}

// ---------------------------------------------------------------- settings <-> controls

void GeneralSettings2::ApplyConfig()
{
	GetConfigHandle().Load(); // show what is on disk
	const auto& gui = GetWxGUIConfig();
	auto& config = GetConfig();

	// --- General
	if (const auto launchMlc = LaunchSettings::GetMLCPath())
		m_mlc_path->SetValue(wxHelper::FromPath(*launchMlc));
	else
		m_mlc_path->SetValue(wxString::FromUTF8(config.mlc_path.GetValue()));

	m_save_window_position_size->SetValue(IsRemembered(gui.window_position));
	m_save_padwindow_position_size->SetValue(IsRemembered(gui.pad_position));
	m_discord_presence->SetValue(gui.use_discord_presence);
	m_fullscreen_menubar->SetValue(gui.fullscreen_menubar);
	m_auto_update->SetValue(gui.check_update);
	m_receive_untested_releases->SetValue(gui.receive_untested_updates);
	m_save_screenshot->SetValue(gui.save_screenshot);
	m_disable_screensaver->SetValue(config.disable_screensaver);
	m_play_boot_sound->SetValue(config.play_boot_sound);
#if BOOST_OS_WINDOWS
	m_msw_theme->SetSelection(gui.msw_theme);
#endif
#if BOOST_OS_LINUX && defined(ENABLE_FERAL_GAMEMODE)
	m_feral_gamemode->SetValue(gui.feral_gamemode);
#endif
#if BOOST_OS_MACOS
	m_disable_screensaver->SetValue(false);
#endif

	m_game_paths->Clear();
	for (const std::string& path : config.game_paths)
		m_game_paths->Append(wxString::FromUTF8(path));

	for (const wxLanguageInfo* language : wxGetApp().GetLanguages())
	{
		if (gui.language == language->Language)
		{
			m_language->SetStringSelection(language->DescriptionNative);
			break;
		}
	}

	// --- Graphics (the vsync choice is filled from config.vsync by HandleGraphicsApiSelection)
	for (size_t i = 0; i < m_api_map.size(); ++i)
	{
		if (m_api_map[i] == config.graphic_api)
		{
			m_graphic_api->SetSelection((int)i);
			break;
		}
	}
	m_overrideGamma->SetValue(config.overrideAppGammaPreference);
	m_overrideGammaValue->SetValue(config.overrideGammaValue);
	const bool srgb = config.userDisplayGamma == 0.0f;
	m_userDisplayisSRGB->SetValue(srgb);
	m_userDisplayGamma->SetValue(srgb ? 2.2f : (float)config.userDisplayGamma);
	m_userDisplayGamma->Enable(!srgb);
	m_async_compile->SetValue(config.async_compile);
	m_gx2drawdone_sync->SetValue(config.gx2drawdone_sync);
	m_upscale_filter->SetSelection(config.upscale_filter);
	m_downscale_filter->SetSelection(config.downscale_filter);
	m_fullscreen_scaling->SetSelection(config.fullscreen_scaling);

	// --- Overlay
	m_overlay_position->SetSelection((int)config.overlay.position);
	m_overlay_font_color->SetColour(wxColour((unsigned long)config.overlay.text_color));
	m_overlay_scale->SetSelection((int)TextScaleToChoice(config.overlay.text_scale));
	m_overlay_fps->SetValue(config.overlay.fps);
	m_overlay_drawcalls->SetValue(config.overlay.drawcalls);
	m_overlay_cpu->SetValue(config.overlay.cpu_usage);
	m_overlay_cpu_per_core->SetValue(config.overlay.cpu_per_core_usage);
	m_overlay_ram->SetValue(config.overlay.ram_usage);
	m_overlay_vram->SetValue(config.overlay.vram_usage);
	m_overlay_debug->SetValue(config.overlay.debug);

	m_notification_position->SetSelection((int)config.notification.position);
	m_notification_font_color->SetColour(wxColour((unsigned long)config.notification.text_color));
	m_notification_scale->SetSelection((int)TextScaleToChoice(config.notification.text_scale));
	m_controller_profile_name->SetValue(config.notification.controller_profiles);
	m_controller_low_battery->SetValue(config.notification.controller_battery);
	m_shader_compiling->SetValue(config.notification.shader_compiling);
	m_friends_data->SetValue(config.notification.friends);

	// --- Audio (slider events also apply latency and volumes)
	if (const char* api = AudioApiName(config.audio_api))
		m_audio_api->SetStringSelection(api);
	SendSliderEvent(m_audio_latency, config.audio_delay);
	m_tv_channels->SetSelection(config.tv_channels);
	m_pad_channels->SetSelection(0); // stereo only
	m_input_channels->SetSelection(0); // mono only

	SendSliderEvent(m_tv_volume, config.tv_volume);
	SelectStoredDevice<OutputDeviceData>(m_tv_device, config.tv_device);
	SendSliderEvent(m_pad_volume, config.pad_volume);
	SelectStoredDevice<OutputDeviceData>(m_pad_device, config.pad_device);
	SendSliderEvent(m_input_volume, config.input_volume);
	SelectStoredDevice<InputDeviceData>(m_input_device, config.input_device);
	SendSliderEvent(m_portal_volume, config.portal_volume);
	SelectStoredDevice<OutputDeviceData>(m_portal_device, config.portal_device);

	// --- Account
	UpdateOnlineAccounts();
	m_active_account->SetSelection(0);
	const uint32 activeId = ActiveSettings::GetPersistentId();
	for (unsigned int i = 0; i < m_active_account->GetCount(); ++i)
	{
		auto* data = static_cast<AccountChoiceData*>(m_active_account->GetClientObject(i));
		if (data && data->Get().GetPersistentId() == activeId)
		{
			m_active_account->SetSelection(i);
			break;
		}
	}
	m_active_service->SetSelection((int)config.GetAccountNetworkService(activeId));
	UpdateAccountInformation();

	// --- Debug
	m_crash_dump->SetSelection((int)config.crash_dump.GetValue());
	m_gdb_port->SetValue(config.gdb_port.GetValue());
}

void GeneralSettings2::StoreConfig()
{
	auto& config = GetConfig();
	auto& gui = GetWxGUIConfig();

	// --- General
	gui.use_discord_presence = m_discord_presence->IsChecked();
	gui.fullscreen_menubar = m_fullscreen_menubar->IsChecked();
	gui.check_update = m_auto_update->IsChecked();
	gui.save_screenshot = m_save_screenshot->IsChecked();
	gui.receive_untested_updates = m_receive_untested_releases->IsChecked();
#if BOOST_OS_LINUX && defined(ENABLE_FERAL_GAMEMODE)
	gui.feral_gamemode = m_feral_gamemode->IsChecked();
#endif
#if BOOST_OS_WINDOWS
	gui.msw_theme = m_msw_theme->GetSelection();
#endif
	config.play_boot_sound = m_play_boot_sound->IsChecked();
	config.disable_screensaver = m_disable_screensaver->IsChecked();
	if (CafeSystem::IsTitleRunning())
		ScreenSaver::SetInhibit(config.disable_screensaver); // applies to the running game right away

	const bool rememberMain = m_save_window_position_size->IsChecked();
	const bool rememberPad = m_save_padwindow_position_size->IsChecked();
	gui.window_position = RememberMarker(rememberMain);
	gui.window_size = RememberMarker(rememberMain);
	gui.pad_position = RememberMarker(rememberPad);
	gui.pad_size = RememberMarker(rememberPad);

	config.game_paths.clear();
	for (const wxString& path : m_game_paths->GetStrings())
		config.game_paths.emplace_back(path.utf8_string());

	if (m_language->GetSelection() == 0)
		gui.language = wxLANGUAGE_DEFAULT;
	else
	{
		const wxString chosen = m_language->GetStringSelection();
		for (const wxLanguageInfo* language : wxGetApp().GetLanguages())
		{
			if (language->DescriptionNative == chosen)
			{
				gui.language = language->Language;
				break;
			}
		}
	}

	// --- Audio
	if (const auto api = AudioApiFromName(m_audio_api->GetStringSelection().utf8_string()))
		config.audio_api = *api;
	config.audio_delay = m_audio_latency->GetValue();
	config.tv_channels = (AudioChannels)m_tv_channels->GetSelection();
	config.pad_channels = kStereo;
	config.input_channels = kMono;
	config.tv_volume = m_tv_volume->GetValue();
	config.pad_volume = m_pad_volume->GetValue();
	config.input_volume = m_input_volume->GetValue();
	config.portal_volume = m_portal_volume->GetValue();
	config.tv_device = SelectedDeviceIdentifier<OutputDeviceData>(m_tv_device);
	config.pad_device = SelectedDeviceIdentifier<OutputDeviceData>(m_pad_device);
	config.input_device = SelectedDeviceIdentifier<InputDeviceData>(m_input_device);
	config.portal_device = SelectedDeviceIdentifier<OutputDeviceData>(m_portal_device);

	// --- Graphics (the device list only holds real devices while no game runs)
	config.graphic_api = m_api_map[m_graphic_api->GetSelection()];
	if (!CafeSystem::IsTitleRunning())
	{
#ifdef ENABLE_VULKAN
		if (config.graphic_api == GraphicAPI::kVulkan)
		{
			const auto* device = SelectedData<RendererDeviceData<VulkanRenderer::DeviceInfo>>(m_graphic_device);
			config.vk_graphic_device_uuid = device ? device->Get().uuid : decltype(config.vk_graphic_device_uuid){};
		}
#endif
	}
	config.gx2drawdone_sync = m_gx2drawdone_sync->IsChecked();
	config.async_compile = m_async_compile->IsChecked();

	// --- Overlay
	config.overlay.position = (ScreenPosition)m_overlay_position->GetSelection();
	config.overlay.text_color = m_overlay_font_color->GetColour().GetRGBA();
	config.overlay.text_scale = ChoiceToTextScale(m_overlay_scale->GetSelection());
	config.overlay.fps = m_overlay_fps->GetValue();
	config.overlay.drawcalls = m_overlay_drawcalls->GetValue();
	config.overlay.cpu_usage = m_overlay_cpu->GetValue();
	config.overlay.cpu_per_core_usage = m_overlay_cpu_per_core->GetValue();
	config.overlay.ram_usage = m_overlay_ram->GetValue();
	config.overlay.vram_usage = m_overlay_vram->GetValue();
	config.overlay.debug = m_overlay_debug->GetValue();

	config.notification.position = (ScreenPosition)m_notification_position->GetSelection();
	config.notification.text_color = m_notification_font_color->GetColour().GetRGBA();
	config.notification.text_scale = ChoiceToTextScale(m_notification_scale->GetSelection());
	config.notification.controller_profiles = m_controller_profile_name->GetValue();
	config.notification.controller_battery = m_controller_low_battery->GetValue();
	config.notification.shader_compiling = m_shader_compiling->GetValue();
	config.notification.friends = m_friends_data->GetValue();

	// --- Account
	config.account.m_persistent_id = GetSelectedAccountPersistentId();

	// --- Debug
	config.crash_dump = (CrashDump)m_crash_dump->GetSelection();
	config.gdb_port = m_gdb_port->GetValue();

	GetConfigHandle().Save();
}

void GeneralSettings2::DisableSettings(bool game_launched)
{
	if (!game_launched)
		return;
	m_graphic_api->Disable();
	m_graphic_api->SetToolTip(_("Graphics API cannot be changed while a title is running"));
	m_graphic_device->Disable();
	m_graphic_device->SetToolTip(_("Graphics Device cannot be changed while a title is running"));
}

// ---------------------------------------------------------------- General

void GeneralSettings2::RescanGamePaths()
{
	m_reload_gamelist = true;
	CafeTitleList::ClearScanPaths();
	for (const wxString& path : m_game_paths->GetStrings())
		CafeTitleList::AddScanPath(wxHelper::MakeFSPath(path));
	CafeTitleList::Refresh();
}

void GeneralSettings2::OnAddPathClicked(wxCommandEvent& event)
{
	wxDirDialog picker(this, _("Select a directory containing games."), wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
	if (picker.ShowModal() != wxID_OK || picker.GetPath().empty())
		return;
	const wxString path = picker.GetPath();
	if (m_game_paths->FindString(path, true) != wxNOT_FOUND)
		return; // already listed
	m_game_paths->Append(path);
	RescanGamePaths();
}

void GeneralSettings2::OnRemovePathClicked(wxCommandEvent& event)
{
	const int selection = m_game_paths->GetSelection();
	if (selection == wxNOT_FOUND)
		return;
	m_game_paths->Delete(selection);
	RescanGamePaths();
}

void GeneralSettings2::OnMLCPathSelect(wxCommandEvent& event)
{
	if (CafeSystem::IsTitleRunning())
	{
		wxMessageBox(_("Can't change MLC path while a game is running!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}
	wxDirDialog picker(this, _("Select MLC directory"), wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
	if (picker.ShowModal() != wxID_OK || picker.GetPath().empty())
		return;

	const fs::path mlc = wxHelper::MakeFSPath(picker.GetPath());
	// an existing MLC only needs a confirmation, an empty folder gets the structure created
	const wxString question = CemuApp::CheckMLCPath(mlc)
								  ? _("Note that changing the MLC location will not transfer any accounts or save files. Are you sure you want to change the path?")
								  : _("The selected directory does not contain the expected MLC structure. Do you want to create a new MLC structure in this directory?\nNote that changing the MLC location will not transfer any accounts or save files.");
	wxMessageDialog confirm(this, question, _("Warning"), wxYES_NO | wxCENTRE | wxICON_WARNING);
	if (confirm.ShowModal() == wxID_NO)
		return;
	// creating the default files also proves the folder is writable
	if (!CemuApp::CreateDefaultMLCFiles(mlc))
	{
		wxMessageBox(_("Failed to create default MLC files in the selected directory. The MLC path has not been changed"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}
	GetConfig().SetMLCPath(mlc);
	FinishMLCChange();
}

void GeneralSettings2::OnMLCPathClear(wxCommandEvent& event)
{
	if (CafeSystem::IsTitleRunning())
	{
		wxMessageBox(_("Can't change MLC path while a game is running!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}
	wxMessageDialog confirm(this, _("Note that changing the MLC location will not transfer any accounts or save files. Are you sure you want to change the path?"), _("Warning"), wxYES_NO | wxCENTRE | wxICON_WARNING);
	if (confirm.ShowModal() == wxID_NO)
		return;
	GetConfig().SetMLCPath("");
	FinishMLCChange();
}

void GeneralSettings2::FinishMLCChange()
{
	// keep everything else changed in the dialog, then restart is needed
	StoreConfig();
	ShowRestartAndQuit(this);
}

// ---------------------------------------------------------------- Graphics

void GeneralSettings2::OnGraphicAPISelected(wxCommandEvent& event)
{
	cemu_assert_debug(!CafeSystem::IsTitleRunning());
	HandleGraphicsApiSelection();
}

void GeneralSettings2::OnUserDisplaySRGBSelected(wxCommandEvent& event)
{
	// sRGB is stored as gamma 0
	const bool srgb = event.IsChecked();
	m_userDisplayGamma->SetValue(2.2f);
	m_userDisplayGamma->Enable(!srgb);
	GetConfig().userDisplayGamma = srgb ? 0.0f : (float)m_userDisplayGamma->GetValue();
}

void GeneralSettings2::HandleGraphicsApiSelection()
{
	int vsync = m_vsync->GetSelection();
	if (vsync == wxNOT_FOUND)
		vsync = GetConfig().vsync;
	m_vsync->Clear();

	auto selectVsync = [this](int choice) {
		if (choice >= 0 && choice < (int)m_vsync->GetCount())
			m_vsync->Select(choice);
	};
	// real devices can only be listed while no renderer exists
	auto fillDevices = [this](const auto& devices, const auto& wantedUuid) {
		using Info = std::decay_t<decltype(devices.front())>;
		m_graphic_device->Enable();
		m_graphic_device->Clear();
		if (CafeSystem::IsTitleRunning())
		{
			cemu_assert(g_renderer != nullptr);
			m_graphic_device->Append(g_renderer->GetDeviceName());
			m_graphic_device->SetSelection(0);
			return;
		}
		if (devices.empty())
			return;
		std::optional<int> configured; // the first device with the stored id, else the first device
		for (size_t i = 0; i < devices.size(); ++i)
		{
			m_graphic_device->Append(devices[i].name, new RendererDeviceData<Info>(devices[i]));
			if (!configured && devices[i].uuid == wantedUuid)
				configured = (int)i;
		}
		m_graphic_device->SetSelection(configured.value_or(0));
	};

	switch (m_api_map[m_graphic_api->GetSelection()])
	{
#ifdef ENABLE_OPENGL
	case GraphicAPI::kOpenGL:
		m_vsync->AppendString(_("Off"));
		m_vsync->AppendString(_("On"));
		m_vsync->Select(OpenGLVsyncChoice(vsync));
		m_graphic_device->Clear();
		m_graphic_device->Disable();
		m_gx2drawdone_sync->Enable();
		m_async_compile->Disable();
		break;
#endif
#ifdef ENABLE_VULKAN
	case GraphicAPI::kVulkan:
	{
		m_gx2drawdone_sync->Disable();
		m_async_compile->Enable();
		m_vsync->AppendString(_("Off"));
		m_vsync->AppendString(_("Double buffering"));
		m_vsync->AppendString(_("Triple buffering"));
#if BOOST_OS_WINDOWS
		m_vsync->AppendString(_("Match emulated display (Experimental)"));
#endif
		selectVsync(vsync);
		const auto devices = CafeSystem::IsTitleRunning() ? decltype(VulkanRenderer::GetDevices()){} : VulkanRenderer::GetDevices();
		fillDevices(devices, GetConfig().vk_graphic_device_uuid);
		break;
	}
#endif
	default:
		break;
	}
}

// ---------------------------------------------------------------- Audio

void GeneralSettings2::OnAudioLatencyChanged(wxCommandEvent& event)
{
	IAudioAPI::s_audioDelay = event.GetInt();
	event.Skip();
}

void GeneralSettings2::OnVolumeChanged(wxCommandEvent& event)
{
	const int volume = event.GetInt();
	const wxObject* source = event.GetEventObject();
	if (source == m_input_volume)
	{
		std::shared_lock lock(g_audioInputMutex);
		if (g_inputAudio)
			g_inputAudio->SetVolume(volume);
	}
	else
	{
		std::shared_lock lock(g_audioMutex);
		if (source == m_pad_volume)
		{
			if (g_padAudio)
				g_padAudio->SetVolume(volume);
			g_padVolume = volume;
		}
		else if (source == m_tv_volume)
		{
			if (g_tvAudio)
				g_tvAudio->SetVolume(volume);
		}
		else if (g_portalAudio)
			g_portalAudio->SetVolume(volume);
	}
	event.Skip();
}

void GeneralSettings2::OnAudioAPISelected(wxCommandEvent& event)
{
	const auto api = AudioApiFromName(m_audio_api->GetStringSelection().utf8_string());
	if (!api)
		return;
	GetConfig().audio_api = *api;
	UpdateAudioDeviceList();
	UpdateAudioDevice();
}

void GeneralSettings2::OnAudioDeviceSelected(wxCommandEvent& event)
{
	UpdateAudioDevice();
}

void GeneralSettings2::OnAudioChannelsSelected(wxCommandEvent& event)
{
	auto* choice = wxDynamicCast(event.GetEventObject(), wxChoice);
	if (!choice || choice->GetSelection() == wxNOT_FOUND)
		return;

	auto& config = GetConfig();
	const auto selected = (AudioChannels)choice->GetSelection();
	AudioChannels* target = choice == m_tv_channels ? &config.tv_channels : choice == m_pad_channels ? &config.pad_channels : nullptr;
	if (target)
	{
		if (*target == selected)
			return;
		*target = selected;
	}

	if (m_game_launched)
		wxMessageBox(_("You have to restart the game in order to apply the new settings."), _("Information"), wxOK | wxCENTRE, this);
	else
		UpdateAudioDevice();
}

void GeneralSettings2::UpdateAudioDeviceList()
{
	wxChoice* outputs[] = { m_tv_device, m_pad_device, m_portal_device };
	for (wxChoice* choice : outputs)
	{
		choice->Clear();
		choice->Append(_("Disabled"));
	}
	m_input_device->Clear();
	m_input_device->Append(_("Disabled"));

	for (const auto& device : IAudioAPI::GetDevices((IAudioAPI::AudioAPI)GetConfig().audio_api))
	{
		for (wxChoice* choice : outputs)
			choice->Append(device->GetName(), new OutputDeviceData(device));
	}
	for (const auto& device : IAudioInputAPI::GetDevices(IAudioInputAPI::Cubeb))
		m_input_device->Append(device->GetName(), new InputDeviceData(device));

	// the TV defaults to the first real device, everything else to disabled
	m_tv_device->SetSelection(m_tv_device->GetCount() > 1 ? 1 : 0);
	m_pad_device->SetSelection(0);
	m_input_device->SetSelection(0);
	m_portal_device->SetSelection(0);
}

void GeneralSettings2::UpdateAudioDevice()
{
	const auto& config = GetConfig();
	const auto api = (IAudioAPI::AudioAPI)config.audio_api;

	std::unique_lock lock(g_audioMutex);
	std::unique_lock inputLock(g_audioInputMutex);

	// recreates one output; returns false when the choice has no selection (nothing after it is touched)
	auto recreate = [&](wxChoice* choice, AudioAPIPtr& target, wxSlider* volume, const char* what, auto open) -> bool {
		if (choice->GetSelection() == wxNOT_FOUND)
		{
			cemu_assert_debug(false);
			return false;
		}
		target.reset();
		const auto* data = SelectedData<OutputDeviceData>(choice);
		if (!data)
			return true;
		try
		{
			target = open(data->Get());
			if (target)
				target->SetVolume(volume->GetValue());
		}
		catch (const std::runtime_error& ex)
		{
			cemuLog_log(LogType::Force, "can't initialize {} audio: {}", what, ex.what());
		}
		return true;
	};
	const sint32 blockSamples = snd_core::AX_SAMPLES_PER_3MS_48KHZ * kAxFramesPerGroup;

	if (!recreate(m_tv_device, g_tvAudio, m_tv_volume, "tv", [&](const IAudioAPI::DeviceDescriptionPtr& device) {
			return IAudioAPI::CreateDevice(api, device, 48000, CemuConfig::AudioChannelsToNChannels(config.tv_channels), blockSamples, 16);
		}))
		return;

	if (!recreate(m_pad_device, g_padAudio, m_pad_volume, "pad", [&](const IAudioAPI::DeviceDescriptionPtr& device) {
			g_padVolume = m_pad_volume->GetValue();
			return IAudioAPI::CreateDevice(api, device, 48000, CemuConfig::AudioChannelsToNChannels(config.pad_channels), blockSamples, 16);
		}))
		return;

	// microphone (always the Cubeb input API)
	if (m_input_device->GetSelection() == wxNOT_FOUND)
	{
		cemu_assert_debug(false);
		return;
	}
	g_inputAudio.reset();
	if (const auto* data = SelectedData<InputDeviceData>(m_input_device))
	{
		try
		{
			g_inputAudio = IAudioInputAPI::CreateDevice(IAudioInputAPI::AudioInputAPI::Cubeb, data->Get(), 32000,
				CemuConfig::AudioChannelsToNChannels(config.input_channels), snd_core::AX_SAMPLES_PER_3MS_32KHZ, 16);
			if (g_inputAudio)
				g_inputAudio->SetVolume(m_input_volume->GetValue());
		}
		catch (const std::runtime_error& ex)
		{
			cemuLog_log(LogType::Force, "can't initialize microphone audio: {}", ex.what());
		}
	}

	// Skylanders portal speaker: 8kHz mono
	recreate(m_portal_device, g_portalAudio, m_portal_volume, "portal", [&](const IAudioAPI::DeviceDescriptionPtr& device) {
		return IAudioAPI::CreateDevice(api, device, 8000, 1, 32, 16);
	});
}

// ---------------------------------------------------------------- Account

Account* GeneralSettings2::GetSelectedAccount()
{
	const int selection = m_active_account->GetSelection();
	if (selection == wxNOT_FOUND)
		return nullptr;
	auto* data = static_cast<AccountChoiceData*>(m_active_account->GetClientObject(selection));
	return data ? &data->Get() : nullptr;
}

uint32 GeneralSettings2::GetSelectedAccountPersistentId()
{
	if (const Account* account = GetSelectedAccount())
		return account->GetPersistentId();
	return GetConfig().account.m_persistent_id.GetInitValue();
}

void GeneralSettings2::UpdateAccountButtons()
{
	m_create_account->Enable((int)m_active_account->GetCount() < kMaxAccounts);
	m_delete_account->Enable(m_active_account->GetCount() > 1);
}

void GeneralSettings2::NotifyAccountListChanged()
{
	wxASSERT(GetParent());
	TellParent(GetParent());
}

void GeneralSettings2::UpdateOnlineAccounts()
{
	m_active_account->Clear();
	for (const Account& account : Account::GetAccounts())
		m_active_account->Append(account.ToString(), new AccountChoiceData(account));
	m_active_account->SetSelection(0);
	UpdateAccountButtons();
	UpdateAccountInformation();
}

void GeneralSettings2::ResetAccountInformation()
{
	m_account_grid->SetSplitterPosition(100);
	m_active_account->SetSelection(0);
	for (auto it = m_account_grid->GetIterator(); !it.AtEnd(); ++it)
		(*it)->SetValueToUnspecified();
	m_account_information->InvalidateBestSize();
#if BOOST_OS_WINDOWS
	m_account_information->OnStateChange(GetBestSize());
#endif
}

void GeneralSettings2::UpdateAccountInformation()
{
	m_account_grid->SetSplitterPosition(100);

	auto showValid = [this](bool valid) {
		m_validate_online->SetBitmap(SmallIcon(valid ? wxBITMAP_PNG_FROM_DATA(PNG_CHECK_YES) : wxBITMAP_PNG_FROM_DATA(PNG_ERROR)));
		const long style = m_validate_online->GetWindowStyleFlag();
		m_validate_online->SetWindowStyleFlag(valid ? (style | wxBORDER_NONE) : (style & ~wxBORDER_NONE));
	};

	const int selection = m_active_account->GetSelection();
	const Account* account = GetSelectedAccount();
	if (!account)
	{
		showValid(false);
		ResetAccountInformation();
		m_online_status->SetLabel(_("No account selected"));
		return;
	}

	m_active_account->SetString(selection, account->ToString());

	m_account_grid->GetProperty(kPropPersistentId)->SetValueFromString(fmt::format("{:x}", account->GetPersistentId()));
	m_account_grid->GetProperty(kPropMiiName)->SetValueFromString(std::wstring(account->GetMiiName()));
	m_account_grid->GetProperty(kPropBirthday)->SetValueFromString(fmt::format("{:04d}-{:02d}-{:02d}", account->GetBirthYear(), account->GetBirthMonth(), account->GetBirthDay()));
	wxPGProperty* gender = m_account_grid->GetProperty(kPropGender);
	gender->SetChoiceSelection(std::min(gender->GetChoices().GetCount() - 1, (uint32)account->GetGender()));
	m_account_grid->GetProperty(kPropEmail)->SetValueFromString(std::string(account->GetEmail()));
	auto* country = dynamic_cast<wxEnumProperty*>(m_account_grid->GetProperty(kPropCountry));
	wxASSERT(country);
	const int countryIndex = country->GetIndexForValue(account->GetCountry());
	country->SetChoiceSelection(countryIndex == wxNOT_FOUND ? 0 : countryIndex);

	// online status line
	const bool filesPresent = ActiveSettings::HasRequiredOnlineFiles();
	const bool linked = account->IsValidOnlineAccount();
	const bool onlineReady = linked && filesPresent;
	if (filesPresent)
		m_online_status->SetLabel(linked ? _("Selected account is a valid online account") : _("Selected account is not linked to a NNID or PNID"));
	else if (NCrypto::OTP_IsPresent() != NCrypto::SEEPROM_IsPresent())
		m_online_status->SetLabel(_("OTP.bin or SEEPROM.bin is missing"));
	else if (NCrypto::OTP_IsPresent())
		m_online_status->SetLabel(_("OTP and SEEPROM present but no certificate files were found"));
	else
		m_online_status->SetLabel(_("Online play is not set up. Follow the guide below to get started"));
	showValid(onlineReady);

	// a network service can only be picked for a working online account
	m_active_service->Enable(onlineReady && !CafeSystem::IsTitleRunning());
	if (onlineReady)
	{
		const NetworkService service = GetConfig().GetAccountNetworkService(account->GetPersistentId());
		m_active_service->SetSelection((int)service);
		// written back so settings.xml always holds the per account choice
		GetConfig().SetAccountSelectedService(account->GetPersistentId(), service);
	}
	else
		m_active_service->SetSelection(0); // offline

	wxString serviceTitle = _("Network service");
	serviceTitle << " (" << wxString::FromUTF8(boost::nowide::narrow(account->GetMiiName())) << ")";
	m_active_service->SetLabel(serviceTitle);

	m_account_grid->InvalidateBestSize();
}

void GeneralSettings2::OnActiveAccountChanged(wxCommandEvent& event)
{
	UpdateAccountInformation();
	m_has_account_change = true;
}

void GeneralSettings2::OnAccountServiceChanged(wxCommandEvent& event)
{
	GetConfig().SetAccountSelectedService(GetSelectedAccountPersistentId(), (NetworkService)m_active_service->GetSelection());
	UpdateAccountInformation();
}

void GeneralSettings2::OnAccountCreate(wxCommandEvent& event)
{
	wxASSERT(Account::HasFreeAccountSlots());
	wxCreateAccountDialog dialog(this);
	if (dialog.ShowModal() == wxID_CANCEL)
		return;

	Account account(dialog.GetPersistentId(), dialog.GetMiiName().ToStdWstring());
	account.Save();
	Account::RefreshAccounts();

	m_active_account->SetSelection(m_active_account->Append(account.ToString(), new AccountChoiceData(account)));
	UpdateAccountInformation();
	UpdateAccountButtons();
	NotifyAccountListChanged();
}

void GeneralSettings2::OnAccountDelete(wxCommandEvent& event)
{
	if (m_active_account->GetCount() == 1)
	{
		wxMessageBox(_("Can't delete the only account!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}
	const int selection = m_active_account->GetSelection();
	const Account* account = GetSelectedAccount();
	if (!account)
		return;

	const std::wstring question = fmt::format(fmt::runtime(_("Are you sure you want to delete the account {} with id {:x}?").ToStdWstring()),
		std::wstring(account->GetMiiName()), account->GetPersistentId());
	if (wxMessageBox(question, _("Confirmation"), wxYES_NO | wxCENTRE | wxICON_QUESTION, this) == wxNO)
		return;

	// the account folder holds account.dat (saves are left alone)
	const fs::path folder = fs::path(account->GetFileName()).parent_path();
	try
	{
		fs::remove_all(folder);
		m_active_account->Delete(selection);
		m_active_account->SetSelection(0);
		Account::RefreshAccounts();
		UpdateAccountInformation();
		UpdateAccountButtons();
		m_has_account_change = true;
	}
	catch (const std::exception& ex)
	{
		const SystemException error(ex);
		cemuLog_log(LogType::Force, error.what());
	}
}

void GeneralSettings2::OnAccountSettingsChanged(wxPropertyGridEvent& event)
{
	wxPGProperty* property = event.GetProperty();
	if (!property)
		return;
	const wxAny value = property->GetValue();
	if (value.IsNull())
		return;
	Account* account = GetSelectedAccount();
	if (!account)
		return;

	const wxString& name = property->GetName();
	bool nameChanged = false;
	if (name == kPropMiiName)
	{
		std::wstring mii = value.As<wxString>().ToStdWstring();
		account->SetMiiName(mii.empty() ? std::wstring(L"default") : mii);
		nameChanged = true;
	}
	else if (name == kPropBirthday)
	{
		const auto parts = SplitDate(value.As<wxString>().ToStdString());
		if (parts.size() == 3)
		{
			account->SetBirthYear(ConvertString<uint16>(parts[0]));
			account->SetBirthMonth(ConvertString<uint8>(parts[1]));
			account->SetBirthDay(ConvertString<uint8>(parts[2]));
		}
	}
	else if (name == kPropGender)
		account->SetGender(value.As<int>());
	else if (name == kPropEmail)
		account->SetEmail(value.As<wxString>().ToStdString());
	else if (name == kPropCountry)
		account->SetCountry(value.As<int>());
	else
		cemu_assert_debug(false);

	account->Save();
	Account::RefreshAccounts();
	UpdateAccountInformation(); // shows what was actually accepted

	if (nameChanged)
		TellParent(GetParent());
}

void GeneralSettings2::OnShowOnlineValidator(wxCommandEvent& event)
{
	const Account* account = GetSelectedAccount();
	if (!account)
		return;
	const OnlineValidator check = account->ValidateOnlineFiles();
	if (check)
		return; // nothing to report

	wxString report;
	report << _("The following error(s) have been found:") << '\n';

	auto describeFile = [&](OnlineValidator::FileState state, const wxString& missing, const wxString& invalid) {
		if (state == OnlineValidator::FileState::Missing)
			report << missing << '\n';
		else if (state == OnlineValidator::FileState::Corrupted)
			report << invalid << '\n';
	};
	describeFile(check.otp, _("otp.bin missing in PixelU directory"), _("otp.bin is invalid"));
	describeFile(check.seeprom, _("seeprom.bin missing in PixelU directory"), _("seeprom.bin is invalid"));

	if (!check.missing_files.empty())
	{
		report << _("Missing certificate and key files:") << '\n';
		const size_t shown = std::min(check.missing_files.size(), kMaxListedMissingFiles);
		for (size_t i = 0; i < shown; ++i)
			report << check.missing_files[i] << '\n';
		if (check.missing_files.size() > shown)
			report << "..." << '\n';
		report << '\n';
	}

	if (!check.valid_account)
	{
		report << _("The currently selected account is not a valid or dumped online account:") << '\n';
		report << GetOnlineAccountErrorMessage(check.account_error);
	}

	wxMessageBox(report, _("Online Status"), wxOK | wxCENTRE | wxICON_INFORMATION);
}

wxString GeneralSettings2::GetOnlineAccountErrorMessage(OnlineAccountError error)
{
	switch (error)
	{
	case OnlineAccountError::kNoAccountId:
		return _("AccountId missing (The account is not connected to a NNID/PNID)");
	case OnlineAccountError::kNoPasswordCached:
		return _("IsPasswordCacheEnabled is set to false (The remember password option on your Wii U must be enabled for this account before dumping it)");
	case OnlineAccountError::kPasswordCacheEmpty:
		return _("AccountPasswordCache is empty (The remember password option on your Wii U must be enabled for this account before dumping it)");
	case OnlineAccountError::kNoPrincipalId:
		return _("PrincipalId missing");
	default:
		return "no error";
	}
}
