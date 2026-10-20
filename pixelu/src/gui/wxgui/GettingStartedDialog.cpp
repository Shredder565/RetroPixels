// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/GettingStartedDialog.h"

#include <wx/sizer.h>
#include <wx/filepicker.h>
#include <wx/statbox.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/button.h>

#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "wxCemuConfig.h"
#include "wxgui/CemuApp.h"
#include "wxgui/DownloadGraphicPacksWindow.h"
#include "wxgui/GraphicPacksWindow2.h"
#include "wxgui/input/InputSettings2.h"

#include "Cafe/TitleList/TitleList.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include "resource/embedded/resources.h"
#endif

#include "wxHelper.h"

// (kept for other code; nothing posts it at the moment)
wxDEFINE_EVENT(EVT_REFRESH_FIRST_PAGE, wxCommandEvent);

namespace
{
	// a bottom row whose buttons sit in the lower right corner
	wxFlexGridSizer* MakeButtonRow(int columns, int growColumn)
	{
		auto* row = new wxFlexGridSizer(0, columns, 0, 0);
		row->AddGrowableCol(growColumn);
		row->AddGrowableRow(0);
		row->SetFlexibleDirection(wxBOTH);
		row->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_ALL);
		return row;
	}

	constexpr int kCornerFlags = wxALIGN_BOTTOM | wxALIGN_RIGHT | wxALL;
}

GettingStartedDialog::GettingStartedDialog(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Getting started"), wxDefaultPosition, {740, 530}, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	m_notebook = new wxSimplebook(this, wxID_ANY);
	m_notebook->AddPage(CreatePage1(), wxEmptyString);
	m_notebook->AddPage(CreatePage2(), wxEmptyString);

	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(m_notebook, 1, wxEXPAND | wxALL, 5);
	SetSizer(layout);
	Centre(wxBOTH);
	Bind(wxEVT_CLOSE_WINDOW, &GettingStartedDialog::OnClose, this);

	ApplySettings();
	UpdateWindowSize();
}

wxPanel* GettingStartedDialog::CreatePage1()
{
	auto* page = new wxPanel(m_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	auto* column = new wxBoxSizer(wxVERTICAL);

	// greeting
	{
		auto* row = new wxBoxSizer(wxHORIZONTAL);
		row->Add(new wxStaticBitmap(page, wxID_ANY, wxICON(M_WND_ICON128)), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		m_page1.staticText11 = new wxStaticText(page, wxID_ANY, _("It looks like you're starting PixelU for the first time.\nThis quick setup assistant will help you get the best experience"));
		m_page1.staticText11->Wrap(-1);
		row->Add(m_page1.staticText11, 0, wxALL, 5);
		column->Add(row, 0, wxALL | wxEXPAND, 5);
	}

	if (ActiveSettings::IsPortableMode())
	{
		m_page1.portableModeInfoText = new wxStaticText(page, wxID_ANY, _("PixelU is running in portable mode"));
		m_page1.portableModeInfoText->Show(true);
		column->Add(m_page1.portableModeInfoText, 0, wxALL, 5);
	}

	// game folder
	{
		m_page1.gamePathBoxSizer = new wxStaticBoxSizer(wxVERTICAL, page, _("Game paths"));
		wxWindow* box = m_page1.gamePathBoxSizer->GetStaticBox();

		m_page1.gamePathText = new wxStaticText(box, wxID_ANY, _("The game path is scanned by PixelU to automatically locate your games, game updates and DLCs. We recommend creating a dedicated directory in which\nyou place all your Wii U game files. Additional paths can be set later in the general settings. All common Wii U game formats are supported."));
		m_page1.gamePathBoxSizer->Add(m_page1.gamePathText, 0, wxALL, 5);

		auto* row = new wxBoxSizer(wxHORIZONTAL);
		m_page1.gamePathText2 = new wxStaticText(box, wxID_ANY, _("Game path"));
		row->Add(m_page1.gamePathText2, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		m_page1.gamePathPicker = new wxDirPickerCtrl(box, wxID_ANY, wxEmptyString, _("Select a folder"));
		row->Add(m_page1.gamePathPicker, 1, wxALL, 5);
		m_page1.gamePathBoxSizer->Add(row, 0, wxEXPAND, 5);

		column->Add(m_page1.gamePathBoxSizer, 0, wxALL | wxEXPAND, 5);
	}

	// graphic packs
	{
		auto* group = new wxStaticBoxSizer(wxVERTICAL, page, _("Graphic packs && mods"));
		wxWindow* box = group->GetStaticBox();
		group->Add(new wxStaticText(box, wxID_ANY, _("Graphic packs improve games by offering the ability to change resolution, increase FPS, tweak visuals or add gameplay modifications.\nGet started by opening the graphic packs configuration window.\n")), 0, wxALL, 5);

		auto* openPacks = new wxButton(box, wxID_ANY, _("Download and configure graphic packs"));
		openPacks->Bind(wxEVT_BUTTON, &GettingStartedDialog::OnConfigureGPs, this);
		group->Add(openPacks, 0, wxALIGN_CENTER | wxALL, 5);

		column->Add(group, 0, wxALL | wxEXPAND, 5);
	}

	{
		auto* buttons = MakeButtonRow(1, 0);
		auto* next = new wxButton(page, wxID_ANY, _("Next"));
		next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_notebook->SetSelection(1); });
		buttons->Add(next, 0, kCornerFlags, 5);
		column->Add(buttons, 1, wxEXPAND, 5);
	}

	page->SetSizer(column);
	return page;
}

wxPanel* GettingStartedDialog::CreatePage2()
{
	auto* page = new wxPanel(m_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
	auto* column = new wxBoxSizer(wxVERTICAL);

	// input
	{
		auto* group = new wxStaticBoxSizer(new wxStaticBox(page, wxID_ANY, _("Input settings")), wxVERTICAL);
		wxWindow* box = group->GetStaticBox();
		group->Add(new wxStaticText(box, wxID_ANY, _("You can configure one controller for each player.\nWe advise you to always use GamePad as emulated input for the first player, since many games expect the GamePad to be present.\nIt is also required for touch functionality.\nThe default global hotkeys are:\nCTRL - show pad screen\nCTRL + TAB - toggle pad screen\nALT + ENTER - toggle fullscreen\nESC - leave fullscreen\n\nIf you're having trouble configuring your controller, make sure to have it in idle state and press calibrate.\nAlso don't set the axis deadzone too low.")), 0, wxALL, 5);

		auto* openInput = new wxButton(box, wxID_ANY, _("Configure input"));
		openInput->Bind(wxEVT_BUTTON, &GettingStartedDialog::OnInputSettings, this);
		group->Add(openInput, 0, wxALIGN_CENTER | wxALL, 5);

		column->Add(group, 0, wxALL | wxEXPAND, 5);
	}

	// options
	{
		auto* group = new wxStaticBoxSizer(new wxStaticBox(page, wxID_ANY, _("Additional options")), wxVERTICAL);
		wxWindow* box = group->GetStaticBox();

		auto* grid = new wxFlexGridSizer(0, 2, 0, 0);
		grid->SetFlexibleDirection(wxBOTH);
		grid->SetNonFlexibleGrowMode(wxFLEX_GROWMODE_SPECIFIED);

		m_page2.fullscreenCheckbox = new wxCheckBox(box, wxID_ANY, _("Start games with fullscreen"));
		grid->Add(m_page2.fullscreenCheckbox, 0, wxALL, 5);
		m_page2.separateCheckbox = new wxCheckBox(box, wxID_ANY, _("Open separate pad screen"));
		grid->Add(m_page2.separateCheckbox, 0, wxALL, 5);
		m_page2.updateCheckbox = new wxCheckBox(box, wxID_ANY, _("Automatically check for updates"));
		grid->Add(m_page2.updateCheckbox, 0, wxALL, 5);
#if BOOST_OS_LINUX
		// only AppImage builds can update themselves
		if (!std::getenv("APPIMAGE"))
			m_page2.updateCheckbox->Disable();
#endif
		group->Add(grid, 1, wxEXPAND, 5);
		column->Add(group, 0, wxALL | wxEXPAND, 5);
	}

	{
		auto* buttons = MakeButtonRow(3, 1);
		auto* back = new wxButton(page, wxID_ANY, _("Previous"));
		back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_notebook->SetSelection(0); });
		buttons->Add(back, 0, kCornerFlags, 5);

		auto* done = new wxButton(page, wxID_ANY, _("Close"));
		done->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
		buttons->Add(done, 1, kCornerFlags, 5);

		column->Add(buttons, 1, wxEXPAND | wxLEFT, 5);
	}

	page->SetSizer(column);
	return page;
}

void GettingStartedDialog::ApplySettings()
{
	const auto& config = GetWxGUIConfig();
	m_page2.fullscreenCheckbox->SetValue(config.fullscreen.GetValue());
	m_page2.updateCheckbox->SetValue(config.check_update.GetValue());
	m_page2.separateCheckbox->SetValue(config.pad_open.GetValue());
}

void GettingStartedDialog::UpdateWindowSize()
{
	for (wxWindow* page : m_notebook->GetChildren())
		page->Layout();
	m_notebook->Layout();
	Layout();
	Fit();
}

void GettingStartedDialog::OnClose(wxCloseEvent& event)
{
	event.Skip();

	auto& guiConfig = GetWxGUIConfig();
	guiConfig.fullscreen = m_page2.fullscreenCheckbox->GetValue();
	guiConfig.check_update = m_page2.updateCheckbox->GetValue();
	guiConfig.pad_open = m_page2.separateCheckbox->GetValue();

	// the chosen game folder is added to the game paths unless it is there already
	const fs::path folder = wxHelper::MakeFSPath(m_page1.gamePathPicker->GetPath());
	std::error_code ec;
	if (folder.empty() || !fs::exists(folder, ec))
		return;
	auto& paths = GetConfig().game_paths;
	const bool known = std::any_of(paths.cbegin(), paths.cend(), [&](const std::string& entry) { return fs::path(entry) == folder; });
	if (!known)
		paths.emplace_back(_pathToUtf8(folder));
}

void GettingStartedDialog::OnConfigureGPs(wxCommandEvent&)
{
	{
		DownloadGraphicPacksWindow download(this);
		download.ShowModal();
	}
	GraphicPacksWindow2::RefreshGraphicPacks();
	GraphicPacksWindow2 packs(this, 0);
	packs.ShowModal();
}

void GettingStartedDialog::OnInputSettings(wxCommandEvent&)
{
	InputSettings2 dialog(this);
	dialog.ShowModal();
}
