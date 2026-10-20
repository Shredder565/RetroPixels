// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <wx/frame.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/slider.h>

#include "Cafe/GameProfile/GameProfile.h"

// "Edit game profile" from the game list: per-title overrides (CPU mode, thread quantum, graphics
// API and shader options, controller profiles). The profile is loaded when the window opens and
// saved when it is destroyed.
class GameProfileWindow : public wxFrame
{
public:
	GameProfileWindow(wxWindow* parent, uint64_t title_id);
	~GameProfileWindow();

private:
	uint64_t m_title_id;
	GameProfile m_game_profile;

	void OnStreamoutSizeChange(wxCommandEvent& event);
	void OnControllerProfileDropdown(wxCommandEvent& event);

	void SetSliderValue(wxSlider* slider, sint32 new_value) const;
	void SetProfileInt(gameProfileIntegerOption_t& option, wxCheckBox* checkbox, sint32 value) const;

	// profile -> controls, controls -> profile (and file)
	void ApplyProfile();
	void SaveProfile();

	// General page
	wxCheckBox *m_load_libs, *m_start_with_padview;
	wxChoice* m_cpu_mode;
	wxChoice* m_thread_quantum;

	// Graphic page
	wxChoice* m_graphic_api;
	wxChoice* m_shader_mul_accuracy;

	// Controller page
	wxComboBox* m_controller_profile[8];
};
