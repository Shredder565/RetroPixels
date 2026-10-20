// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "config/CemuConfig.h"
#include <wx/collpane.h>
#include <wx/propgrid/propgrid.h>
#include <Cafe/Account/Account.h>

class wxCheckBox;
class wxChoice;
class wxColourPickerCtrl;
class wxListBox;
class wxNotebook;
class wxRadioBox;
class wxSlider;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;

// sent to the parent (the main window) when the account list or the active account changed
wxDECLARE_EVENT(wxEVT_ACCOUNTLIST_REFRESH, wxCommandEvent);

// Options > General settings: the General, Graphics, Audio, Overlay, Account and Debug tabs.
// Most values are written to the settings when the dialog closes; a few (vsync, gamma, filters,
// audio devices and volumes) take effect immediately.
class GeneralSettings2 : public wxDialog
{
public:
	GeneralSettings2(wxWindow* parent, bool game_launched);
	~GeneralSettings2();

	// a game path was added or removed
	[[nodiscard]] bool ShouldReloadGamelist() const { return m_reload_gamelist; }
	[[nodiscard]] bool MLCModified() const { return m_mlc_modified; }
	void OnClose(wxCloseEvent& event);

private:
	bool m_reload_gamelist = false;
	bool m_mlc_modified = false;
	bool m_game_launched;
	bool m_has_account_change = false; // tell the main window on close
	std::vector<GraphicAPI> m_api_map; // graphics API choice index -> API

	// --- pages
	wxPanel* AddGeneralPage(wxNotebook* notebook);
	wxPanel* AddGraphicsPage(wxNotebook* notebook);
	wxPanel* AddAudioPage(wxNotebook* notebook);
	wxPanel* AddOverlayPage(wxNotebook* notebook);
	wxPanel* AddAccountPage(wxNotebook* notebook);
	wxPanel* AddDebugPage(wxNotebook* notebook);

	// settings <-> controls
	void ApplyConfig();
	void StoreConfig();
	void DisableSettings(bool game_launched);

	// --- General
	wxChoice* m_language;
	wxCheckBox *m_save_window_position_size, *m_save_padwindow_position_size;
	wxCheckBox *m_discord_presence, *m_fullscreen_menubar;
	wxCheckBox *m_auto_update, *m_receive_untested_releases, *m_save_screenshot;
	wxCheckBox* m_disable_screensaver;
	wxCheckBox* m_play_boot_sound;
#if BOOST_OS_WINDOWS
	wxChoice* m_msw_theme;
#endif
#if BOOST_OS_LINUX && defined(ENABLE_FERAL_GAMEMODE)
	wxCheckBox* m_feral_gamemode;
#endif
	wxListBox* m_game_paths;
	wxTextCtrl* m_mlc_path;

	void OnAddPathClicked(wxCommandEvent& event);
	void OnRemovePathClicked(wxCommandEvent& event);
	void RescanGamePaths();
	void OnMLCPathSelect(wxCommandEvent& event);
	void OnMLCPathClear(wxCommandEvent& event);
	void FinishMLCChange();

	// --- Graphics
	wxChoice *m_graphic_api, *m_graphic_device;
	wxChoice* m_vsync;
	wxCheckBox* m_overrideGamma;
	wxSpinCtrlDouble* m_overrideGammaValue;
	wxSpinCtrlDouble* m_userDisplayGamma;
	wxCheckBox* m_userDisplayisSRGB;
	wxCheckBox *m_async_compile, *m_gx2drawdone_sync;
#ifdef ENABLE_METAL
	wxCheckBox* m_force_mesh_shaders;
#endif
	wxRadioBox *m_upscale_filter, *m_downscale_filter, *m_fullscreen_scaling;

	void OnGraphicAPISelected(wxCommandEvent& event);
	void OnUserDisplaySRGBSelected(wxCommandEvent& event);
	// fills vsync modes and devices for the selected API
	void HandleGraphicsApiSelection();

	// --- Overlay
	wxChoice *m_overlay_position, *m_notification_position, *m_overlay_scale, *m_notification_scale;
	wxCheckBox *m_controller_profile_name, *m_controller_low_battery, *m_shader_compiling, *m_friends_data;
	wxCheckBox *m_overlay_fps, *m_overlay_drawcalls, *m_overlay_cpu, *m_overlay_cpu_per_core, *m_overlay_ram, *m_overlay_vram, *m_overlay_debug;
	wxColourPickerCtrl *m_overlay_font_color, *m_notification_font_color;

	// --- Audio
	wxChoice* m_audio_api;
	wxSlider* m_audio_latency;
	wxSlider *m_tv_volume, *m_pad_volume, *m_input_volume, *m_portal_volume;
	wxChoice *m_tv_channels, *m_pad_channels, *m_input_channels;
	wxChoice *m_tv_device, *m_pad_device, *m_input_device, *m_portal_device;

	void OnAudioLatencyChanged(wxCommandEvent& event);
	void OnVolumeChanged(wxCommandEvent& event);
	void OnAudioAPISelected(wxCommandEvent& event);
	void OnAudioDeviceSelected(wxCommandEvent& event);
	void OnAudioChannelsSelected(wxCommandEvent& event);
	// recreates the global output/input devices from the selections
	void UpdateAudioDevice();
	// fills the device choices for the configured audio API
	void UpdateAudioDeviceList();

	// --- Account
	wxButton *m_create_account, *m_delete_account;
	wxChoice* m_active_account;
	wxRadioBox* m_active_service;
	wxCollapsiblePane* m_account_information;
	wxPropertyGrid* m_account_grid;
	wxBitmapButton* m_validate_online;
	wxStaticText* m_online_status;

	void OnAccountCreate(wxCommandEvent& event);
	void OnAccountDelete(wxCommandEvent& event);
	void OnAccountSettingsChanged(wxPropertyGridEvent& event);
	void OnActiveAccountChanged(wxCommandEvent& event);
	void OnShowOnlineValidator(wxCommandEvent& event);
	void OnAccountServiceChanged(wxCommandEvent& event);
	static wxString GetOnlineAccountErrorMessage(OnlineAccountError error);

	uint32 GetSelectedAccountPersistentId();
	Account* GetSelectedAccount();
	void UpdateAccountButtons();
	void ResetAccountInformation();
	void UpdateAccountInformation();
	void UpdateOnlineAccounts();
	void NotifyAccountListChanged();

	// --- Debug
	wxChoice* m_crash_dump;
	wxSpinCtrl* m_gdb_port;
#ifdef ENABLE_METAL
	wxTextCtrl* m_gpu_capture_dir;
	wxCheckBox* m_framebuffer_fetch;
#endif
};
