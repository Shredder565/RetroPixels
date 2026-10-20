// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/GameProfileWindow.h"

#include <wx/statbox.h>
#include <wx/sizer.h>
#include <wx/panel.h>
#include <wx/notebook.h>
#include <wx/stattext.h>
#include <wx/wupdlock.h>
#include <wx/slider.h>

#include "wxgui/helpers/wxHelpers.h"
#include "input/InputManager.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif

namespace
{
	const int kLabelFlags = wxALIGN_CENTER_VERTICAL | wxALL;

	wxFlexGridSizer* MakeGrid(int columns, bool flexibleBoth = true)
	{
		auto* grid = new wxFlexGridSizer(0, columns, 0, 0);
		if (flexibleBoth)
			grid->SetFlexibleDirection(wxBOTH);
		grid->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);
		return grid;
	}

	template<size_t N>
	wxChoice* MakeChoice(wxWindow* parent, const wxString (&items)[N], const wxString& tip = {})
	{
		auto* choice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, (int)N, items);
		if (!tip.empty())
			choice->SetToolTip(tip);
		return choice;
	}

	// a notebook page holding one vertical sizer
	wxPanel* AddPage(wxNotebook* book, wxSizer* content, wxPanel* page, const wxString& title, bool select)
	{
		page->SetSizer(content);
		page->Layout();
		content->Fit(page);
		book->AddPage(page, title, select);
		return page;
	}

	// list position of each CPU mode in the "Mode" choice
	int CpuModeToChoice(CPUMode mode)
	{
		switch (mode)
		{
		case CPUMode::SinglecoreInterpreter: return 0;
		case CPUMode::SinglecoreRecompiler: return 1;
		case CPUMode::DualcoreRecompiler:
		case CPUMode::MulticoreRecompiler: return 2;
		default: return 3;
		}
	}

	CPUMode ChoiceToCpuMode(int index)
	{
		switch (index)
		{
		case 0: return CPUMode::SinglecoreInterpreter;
		case 1: return CPUMode::SinglecoreRecompiler;
		case 2: return CPUMode::MulticoreRecompiler;
		default: return CPUMode::Auto;
		}
	}
}

GameProfileWindow::GameProfileWindow(wxWindow* parent, uint64_t title_id)
	: wxFrame(parent, wxID_ANY, _("Edit game profile"), wxDefaultPosition, wxSize{390, 350},
			  wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER | wxTAB_TRAVERSAL | wxSYSTEM_MENU),
	  m_title_id(title_id)
{
	SetIcon(wxICON(X_GAME_PROFILE));

	m_game_profile.Reset();
	m_game_profile.Load(title_id);

	SetSizeHints(wxDefaultSize, wxDefaultSize);
	auto* book = new wxNotebook(this, wxID_ANY);

	// General: libraries, GamePad view, CPU
	{
		auto* page = new wxPanel(book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
		auto* column = new wxBoxSizer(wxVERTICAL);

		auto* general = new wxStaticBoxSizer(new wxStaticBox(page, wxID_ANY, _("General")), wxVERTICAL);
		{
			wxWindow* box = general->GetStaticBox();
			m_load_libs = new wxCheckBox(box, wxID_ANY, _("Load shared libraries"));
			m_load_libs->SetToolTip(_("EXPERT OPTION\nThis option will load libraries from the cafeLibs directory"));
			general->Add(m_load_libs, 0, wxALL, 5);

			m_start_with_padview = new wxCheckBox(box, wxID_ANY, _("Launch with gamepad view"));
			m_start_with_padview->SetToolTip(_("Games will be launched with gamepad view toggled as default. The view can be toggled with CTRL + TAB"));
			general->Add(m_start_with_padview, 0, wxALL, 5);
		}
		column->Add(general, 0, wxEXPAND, 5);

		auto* cpu = new wxStaticBoxSizer(new wxStaticBox(page, wxID_ANY, _("CPU")), wxVERTICAL);
		{
			wxWindow* box = cpu->GetStaticBox();
			auto* grid = MakeGrid(2);

			grid->Add(new wxStaticText(box, wxID_ANY, _("Mode")), 0, kLabelFlags, 5);
			const wxString modes[] = {_("Single-core interpreter"), _("Single-core recompiler"), _("Multi-core recompiler"), _("Auto (recommended)")};
			m_cpu_mode = MakeChoice(box, modes, _("Set the CPU emulation mode"));
			grid->Add(m_cpu_mode, 0, wxALL, 5);

			grid->Add(new wxStaticText(box, wxID_ANY, _("Thread quantum")), 0, kLabelFlags, 5);
			auto* quantumRow = MakeGrid(2);
			const wxString quanta[] = {"20000", "45000", "60000", "80000", "100000"};
			m_thread_quantum = MakeChoice(box, quanta, _("EXPERT OPTION\nSet the maximum thread slice runtime (in virtual cycles)"));
			m_thread_quantum->SetMinSize(wxSize(85, -1));
			quantumRow->Add(m_thread_quantum, 0, wxALL, 5);
			quantumRow->Add(new wxStaticText(box, wxID_ANY, _("cycles")), 0, kLabelFlags, 5);
			grid->Add(quantumRow, 0, wxEXPAND, 5);

			cpu->Add(grid, 0, wxEXPAND, 5);
		}
		column->Add(cpu, 0, wxEXPAND, 5);

		AddPage(book, column, page, _("General"), true);
	}

	// Graphic: API and shader options
	{
		auto* page = new wxPanel(book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
		auto* column = new wxBoxSizer(wxVERTICAL);
		auto* grid = MakeGrid(2, false);

		grid->Add(new wxStaticText(page, wxID_ANY, _("Graphics API")), 0, kLabelFlags, 5);
		// first entry: no override
		const wxString apis[] = {"", "OpenGL", "Vulkan",
#ifdef ENABLE_METAL
								 "Metal"
#endif
		};
		m_graphic_api = MakeChoice(page, apis);
		grid->Add(m_graphic_api, 0, wxALL, 5);

		const wxString falseTrue[] = {_("false"), _("true")};
		grid->Add(new wxStaticText(page, wxID_ANY, _("Shader multiplication accuracy")), 0, kLabelFlags, 5);
		m_shader_mul_accuracy = MakeChoice(page, falseTrue, _("EXPERT OPTION\nControls the accuracy of floating point multiplication in shaders.\n\nRecommended: true"));
		grid->Add(m_shader_mul_accuracy, 0, wxALL, 5);

#ifdef ENABLE_METAL
		grid->Add(new wxStaticText(page, wxID_ANY, _("Shader fast math")), 0, kLabelFlags, 5);
		m_shader_fast_math = MakeChoice(page, falseTrue, _("EXPERT OPTION\nEnables fast math for all shaders. May (rarely) cause graphical bugs.\n\nMetal only\n\nRecommended: true"));
		grid->Add(m_shader_fast_math, 0, wxALL, 5);

		grid->Add(new wxStaticText(page, wxID_ANY, _("Metal buffer cache mode")), 0, kLabelFlags, 5);
		const wxString cacheModes[] = {_("auto"), _("device private"), _("device shared"), _("host")};
		m_metal_buffer_cache_mode = MakeChoice(page, cacheModes, _("EXPERT OPTION\nDecides how the buffer cache memory will be managed.\n\nMetal only\n\nRecommended: auto"));
		grid->Add(m_metal_buffer_cache_mode, 0, wxALL, 5);

		grid->Add(new wxStaticText(page, wxID_ANY, _("Position invariance")), 0, kLabelFlags, 5);
		const wxString invariance[] = {_("auto"), _("false"), _("true")};
		m_position_invariance = MakeChoice(page, invariance, _("EXPERT OPTION\nDisables most optimizations for vertex positions. May fix polygon cutouts or flickering in some games.\n\nMetal only\n\nRecommended: auto"));
		grid->Add(m_position_invariance, 0, wxALL, 5);
#endif
		column->Add(grid, 0, wxEXPAND, 5);
		AddPage(book, column, page, _("Graphic"), false);
	}

	// Controller: a forced input profile per controller
	{
		auto* page = new wxPanel(book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
		auto* column = new wxBoxSizer(wxVERTICAL);
		auto* grid = MakeGrid(2);
		for (int i = 0; i < 8; ++i)
		{
			grid->Add(new wxStaticText(page, wxID_ANY, formatWxString(_("Controller {}"), i + 1)), 0, kLabelFlags, 5);
			auto* profile = new wxComboBox(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_DROPDOWN | wxCB_READONLY);
			profile->SetMinSize(wxSize(250, -1));
			profile->Bind(wxEVT_COMBOBOX_DROPDOWN, &GameProfileWindow::OnControllerProfileDropdown, this);
			profile->SetToolTip(_("Forces a given controller profile"));
			grid->Add(profile, 0, wxALL, 5);
			m_controller_profile[i] = profile;
		}
		column->Add(grid, 0, wxEXPAND, 5);
		AddPage(book, column, page, _("Controller"), false);
	}

	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(book, 1, wxEXPAND | wxALL, 5);
	SetSizer(layout);
	Layout();
	Centre(wxBOTH);

	ApplyProfile();
}

GameProfileWindow::~GameProfileWindow()
{
	SaveProfile();
}

void GameProfileWindow::OnStreamoutSizeChange(wxCommandEvent& event)
{
	// a slider's client data is the label that shows its value
	auto* slider = wxDynamicCast(event.GetEventObject(), wxSlider);
	wxASSERT(slider);
	auto* label = wxDynamicCast(slider->GetClientData(), wxStaticText);
	wxASSERT(label);
	label->SetLabelText(fmt::format("{} MB", slider->GetValue()));
	event.Skip();
}

void GameProfileWindow::OnControllerProfileDropdown(wxCommandEvent& event)
{
	// refill with the current profiles (plus "none") right before the list opens
	auto* box = wxDynamicCast(event.GetEventObject(), wxComboBox);
	wxASSERT(box);
	wxWindowUpdateLocker freeze(box);

	const wxString current = box->GetStringSelection();
	box->Clear();
	box->Append(wxEmptyString);
	for (const auto& name : InputManager::get_profiles())
		box->Append(wxString::FromUTF8(name));
	box->SetStringSelection(current);
}

void GameProfileWindow::SetProfileInt(gameProfileIntegerOption_t& option, wxCheckBox* checkbox, sint32 value) const
{
	option.isPresent = checkbox->GetValue();
	if (option.isPresent)
		option.value = value;
}

void GameProfileWindow::ApplyProfile()
{
	const GameProfile& profile = m_game_profile;
	if (profile.m_gameName)
		SetTitle(_("Edit game profile") + " - " + wxString::FromUTF8(profile.m_gameName.value()));

	m_load_libs->SetValue(profile.m_loadSharedLibraries.value());
	m_start_with_padview->SetValue(profile.m_startWithPadView);

	m_cpu_mode->SetSelection(CpuModeToChoice(profile.m_cpuMode.value()));
	// a quantum that isn't in the list leaves the choice empty
	m_thread_quantum->SetStringSelection(fmt::format("{}", profile.m_threadQuantum));

	// entry 0 is "no override", then one per GraphicAPI value
	m_graphic_api->SetSelection(profile.m_graphics_api.has_value() ? 1 + (int)profile.m_graphics_api.value() : 0);
	m_shader_mul_accuracy->SetSelection((int)profile.m_accurateShaderMul);
#ifdef ENABLE_METAL
	m_shader_fast_math->SetSelection((int)profile.m_shaderFastMath);
	m_metal_buffer_cache_mode->SetSelection((int)profile.m_metalBufferCacheMode);
	m_position_invariance->SetSelection((int)profile.m_positionInvariance);
#endif

	const auto profiles = InputManager::get_profiles();
	for (wxComboBox* box : m_controller_profile)
	{
		box->Clear();
		for (const auto& name : profiles)
			box->Append(wxString::FromUTF8(name));
	}
	for (int i = 0; i < InputManager::kMaxController; ++i)
	{
		const auto& forced = profile.m_controllerProfile[i];
		if (forced.has_value())
			m_controller_profile[i]->SetStringSelection(wxString::FromUTF8(forced.value()));
		else
			m_controller_profile[i]->SetSelection(wxNOT_FOUND);
	}
}

void GameProfileWindow::SaveProfile()
{
	GameProfile& profile = m_game_profile;
	profile.Reset();

	profile.m_loadSharedLibraries = m_load_libs->GetValue();
	profile.m_startWithPadView = m_start_with_padview->GetValue();
	profile.m_cpuMode = ChoiceToCpuMode(m_cpu_mode->GetSelection());

	const wxString quantum = m_thread_quantum->GetStringSelection();
	if (!quantum.empty())
		profile.m_threadQuantum = std::clamp<uint32>(ConvertString<uint32>(quantum.ToStdString()), 5000, 536870912);

	auto mul = (AccurateShaderMulOption)m_shader_mul_accuracy->GetSelection();
	if (mul != AccurateShaderMulOption::False && mul != AccurateShaderMulOption::True)
		mul = AccurateShaderMulOption::True; // nothing selected
	profile.m_accurateShaderMul = mul;
#ifdef ENABLE_METAL
	profile.m_shaderFastMath = (bool)m_shader_fast_math->GetSelection();
	profile.m_metalBufferCacheMode = (MetalBufferCacheMode)m_metal_buffer_cache_mode->GetSelection();
	profile.m_positionInvariance = (PositionInvariance)m_position_invariance->GetSelection();
#endif

	const int api = m_graphic_api->GetSelection();
	if (api == 0)
		profile.m_graphics_api = {};
	else
		profile.m_graphics_api = (GraphicAPI)(api - 1);

	for (int i = 0; i < 8; ++i)
	{
		auto& forced = profile.m_controllerProfile[i];
		const wxComboBox* box = m_controller_profile[i];
		const wxString name = box->GetSelection() == wxNOT_FOUND ? wxString() : box->GetStringSelection();
		if (name.empty())
			forced.reset();
		else
			forced = name.utf8_string();
	}

	profile.Save(m_title_id);
}

void GameProfileWindow::SetSliderValue(wxSlider* slider, sint32 new_value) const
{
	// moves the slider and lets its handler update the label
	wxASSERT(slider);
	slider->SetValue(new_value);

	wxCommandEvent changed(wxEVT_SLIDER, slider->GetId());
	changed.SetEventObject(slider);
	changed.SetClientData((void*)IsFrozen());
	wxPostEvent(slider->GetEventHandler(), changed);
}
