// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/InputSettings2.h"

#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/gbsizer.h>
#include <wx/notebook.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/wupdlock.h>

#include "config/ActiveSettings.h"
#include "input/ControllerFactory.h"
#include "input/InputManager.h"
#include "util/EventService.h"
#include "wxgui/helpers/wxCustomData.h"
#include "wxgui/helpers/wxHelpers.h"
#include "wxgui/input/InputAPIAddWindow.h"
#include "wxgui/input/panels/ClassicControllerInputPanel.h"
#include "wxgui/input/panels/ProControllerInputPanel.h"
#include "wxgui/input/panels/VPADInputPanel.h"
#include "wxgui/input/panels/WiimoteInputPanel.h"
#include "wxgui/input/settings/DefaultControllerSettings.h"
#include "wxgui/input/settings/WiimoteControllerSettings.h"

#ifdef HAS_BLUEZ
#include "wxgui/input/PairingDialog.h"
#endif

#include "resource/embedded/resources.h"

// read by the window system so game input is ignored while this dialog is open
bool g_inputConfigWindowHasFocus = false;

// widgets and state of one controller tab, stored as the tab's client object
struct ControllerPage
{
	EmulatedControllerPtr emulated;

	wxComboBox* profiles;
	wxButton* profileLoad;
	wxButton* profileSave;
	wxButton* profileDelete;
	wxStaticText* profileStatus;

	wxComboBox* emulatedType;

	wxComboBox* controllers;
	wxButton* addController;
	wxButton* removeController;
	wxButton* settings;
	wxButton* calibrate;
	wxButton* clear;
	wxBitmapButton* connection;

	wxBoxSizer* panelArea;
	std::array<InputPanel*, EmulatedController::Type::MAX> panels{};
};

namespace
{
	using PageData = wxCustomData<ControllerPage>;
	using ControllerItem = wxCustomData<ControllerPtr>;

	ControllerPage* PageOf(wxWindow* page)
	{
		auto* data = static_cast<PageData*>(page->GetClientObject());
		return data ? &data->ref() : nullptr;
	}

	ControllerPtr ItemController(wxComboBox* list, int index)
	{
		if (index == wxNOT_FOUND)
			return {};
		auto* item = static_cast<ControllerItem*>(list->GetClientObject(index));
		return item ? item->ref() : ControllerPtr{};
	}

	ControllerPtr SelectedController(wxComboBox* list)
	{
		return ItemController(list, list->GetSelection());
	}

	wxString TypeName(EmulatedController::Type type)
	{
		return wxString::FromUTF8(EmulatedController::type_to_string(type));
	}

	// keyboards have nothing to tune
	bool HasSettingsDialog(InputAPI::Type api)
	{
		return api != InputAPI::Keyboard;
	}

	InputPanel* CreatePanel(EmulatedController::Type type, wxWindow* parent)
	{
		switch (type)
		{
		case EmulatedController::Type::VPAD: return new VPADInputPanel(parent);
		case EmulatedController::Pro: return new ProControllerInputPanel(parent);
		case EmulatedController::Classic: return new ClassicControllerInputPanel(parent);
		case EmulatedController::Wiimote: return new WiimoteInputPanel(parent);
		default: return nullptr;
		}
	}

	void ShowStatus(wxStaticText* status, const wxString& text, bool success)
	{
		status->SetLabelText(text);
		status->SetForegroundColour(wxTheColourDatabase->Find(success ? "SUCCESS" : "ERROR"));
	}

	wxBitmap ThemedIcon(const uint8* png, size_t size)
	{
		return wxHelper::LoadThemedBitmapFromPNG(png, size, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
	}
}

InputSettings2::InputSettings2(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Input settings"))
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);
	g_inputConfigWindowHasFocus = true;

	m_connected = ThemedIcon(INPUT_CONNECTED_png, sizeof(INPUT_CONNECTED_png));
	m_disconnected = ThemedIcon(INPUT_DISCONNECTED_png, sizeof(INPUT_DISCONNECTED_png));
	m_low_battery = ThemedIcon(INPUT_LOW_BATTERY_png, sizeof(INPUT_LOW_BATTERY_png));

	// tabs are filled in when first shown
	m_notebook = new wxNotebook(this, wxID_ANY);
	for (size_t i = 0; i < InputManager::kMaxController; ++i)
	{
		auto* tab = new wxPanel(m_notebook, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
		tab->SetClientObject(nullptr); // makes the tab use client objects, not client data
		m_notebook->AddPage(tab, formatWxString(_("Controller {}"), i + 1));
	}
	m_notebook->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent& event) {
		initialize_page(event.GetSelection());
		update_state();
		event.Skip();
	});

	auto* layout = new wxBoxSizer(wxVERTICAL);
	layout->Add(m_notebook, 1, wxEXPAND);

	m_notebook->SetSelection(0);
	wxWindow* firstTab = initialize_page(0);

	// size the dialog for the largest panel (the gamepad), then hide it again
	ControllerPage& first = *PageOf(firstTab);
	auto* sizingPanel = new VPADInputPanel(firstTab);
	first.panels[EmulatedController::Type::VPAD] = sizingPanel;
	first.panelArea->Add(sizingPanel, 0, wxEXPAND);
	sizingPanel->Show();
	firstTab->Layout();

	SetSizer(layout);
	Layout();
	Fit();
	sizingPanel->Hide();

	update_state();

	Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
		// feed the visible panel for binding and the stick previews
		ControllerPage& page = get_current_page_data();
		InputPanel* panel = CurrentPanel();
		if (!page.emulated || !panel)
			return;
		if (const auto controller = get_active_controller())
			panel->on_timer(page.emulated, controller);
	});
	m_timer = new wxTimer(this);
	m_timer->Start(25);

	m_controller_changed = EventService::instance().connect<Events::ControllerChanged>(&InputSettings2::OnControllersChanged, this);
}

InputSettings2::~InputSettings2()
{
	m_controller_changed.disconnect();
	g_inputConfigWindowHasFocus = false;
	m_timer->Stop();
	InputManager::instance().save();
}

ControllerPage& InputSettings2::get_current_page_data() const
{
	ControllerPage* page = PageOf(m_notebook->GetCurrentPage());
	wxASSERT(page);
	return *page;
}

std::shared_ptr<ControllerBase> InputSettings2::get_active_controller() const
{
	return SelectedController(get_current_page_data().controllers);
}

InputPanel* InputSettings2::CurrentPanel() const
{
	const ControllerPage& page = get_current_page_data();
	if (!page.emulated)
		return nullptr;
	return page.panels[page.emulated->type()];
}

wxWindow* InputSettings2::initialize_page(size_t index)
{
	wxWindow* tab = m_notebook->GetPage(index);
	if (PageOf(tab))
		return tab;

	// clicks on empty space end a pending binding
	tab->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
		event.Skip();
		if (InputPanel* panel = CurrentPanel())
			panel->on_left_click(event);
	});

	ControllerPage page{};
	page.emulated = InputManager::instance().get_controller(index);

	wxWindowUpdateLocker freeze(tab);
	auto* grid = new wxGridBagSizer();
	const int textFlags = wxALIGN_CENTER_VERTICAL | wxALL;
	auto addLine = [&](int row) { grid->Add(new wxStaticLine(tab), wxGBPosition(row, 0), wxGBSpan(1, 6), wxEXPAND); };

	// row 0: profile
	{
		grid->Add(new wxStaticText(tab, wxID_ANY, _("Profile")), wxGBPosition(0, 0), wxDefaultSpan, textFlags, 5);
		page.profiles = new wxComboBox(tab, wxID_ANY, kDefaultProfileName);
		grid->Add(page.profiles, wxGBPosition(0, 1), wxDefaultSpan, textFlags | wxEXPAND, 5);
#if BOOST_OS_LINUX || BOOST_OS_BSD
		// wxGTK cannot open the dropdown of an empty combo box, so fill it now
		FillProfileList(page.profiles);
#endif
		if (page.emulated && page.emulated->has_profile_name())
			page.profiles->SetValue(page.emulated->get_profile_name());

		int column = 2;
		for (auto [button, label] : {std::pair{&page.profileLoad, _("Load")}, std::pair{&page.profileSave, _("Save")}, std::pair{&page.profileDelete, _("Delete")}})
		{
			*button = new wxButton(tab, wxID_ANY, label);
			(*button)->Disable();
			grid->Add(*button, wxGBPosition(0, column++), wxDefaultSpan, textFlags, 5);
		}

		page.profileStatus = new wxStaticText(tab, wxID_ANY, _("controller set by gameprofile. changes won't be saved permanently!"));
		page.profileStatus->SetMinSize(wxSize(200, -1));
		page.profileStatus->Wrap(200);
		grid->Add(page.profileStatus, wxGBPosition(0, 5), wxDefaultSpan, textFlags | wxRESERVE_SPACE_EVEN_IF_HIDDEN, 5);
		if (InputManager::instance().is_gameprofile_set(index))
			page.profileStatus->SetForegroundColour(wxTheColourDatabase->Find("ERROR"));
		else
			page.profileStatus->Hide();

		page.profileLoad->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { LoadProfile(); });
		page.profileSave->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveProfile(); });
		page.profileDelete->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DeleteProfile(); });
		wxComboBox* profiles = page.profiles;
		profiles->Bind(wxEVT_COMBOBOX_DROPDOWN, [this, profiles](wxCommandEvent&) { FillProfileList(profiles); });
		profiles->Bind(wxEVT_TEXT, &InputSettings2::OnProfileTextChanged, this);
	}
	addLine(1);

	// row 2: emulated controller type
	{
		grid->Add(new wxStaticText(tab, wxID_ANY, _("Emulated controller")), wxGBPosition(2, 0), wxDefaultSpan, textFlags, 5);
		wxComboBox* types = new wxComboBox(tab, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
		types->SetMinSize(wxSize(200, -1));
		types->Bind(wxEVT_COMBOBOX_DROPDOWN, [this, types](wxCommandEvent&) { FillEmulatedTypeList(types); });
		types->Bind(wxEVT_COMBOBOX, &InputSettings2::OnEmulatedTypeChosen, this);
		types->AppendString(_("Disabled"));
		types->SetSelection(0);
		grid->Add(types, wxGBPosition(2, 1), wxDefaultSpan, textFlags | wxEXPAND, 5);
		page.emulatedType = types;
	}
	addLine(3);

	// rows 4-5: physical controllers
	{
		grid->Add(new wxStaticText(tab, wxID_ANY, _("Controller")), wxGBPosition(4, 0), wxDefaultSpan, textFlags, 5);
		wxComboBox* list = new wxComboBox(tab, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
		list->Bind(wxEVT_COMBOBOX, &InputSettings2::OnControllerChosen, this);
		// opening an empty list goes straight to adding one
		list->Bind(wxEVT_COMBOBOX_DROPDOWN, [this, list](wxCommandEvent&) {
			if (list->GetCount() != 0)
				return;
			AddController();
			list->SetSelection(0);
		});
		list->SetMinSize(wxSize(300, -1));
		grid->Add(list, wxGBPosition(4, 1), wxDefaultSpan, textFlags | wxEXPAND, 5);
		page.controllers = list;

		auto* addRemove = new wxBoxSizer(wxHORIZONTAL);
		page.addController = new wxButton(tab, wxID_ANY, " + ", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
		page.addController->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddController(); });
		addRemove->Add(page.addController, 0, wxALL, 5);
		page.removeController = new wxButton(tab, wxID_ANY, "  -  ", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
		page.removeController->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RemoveController(); });
		addRemove->Add(page.removeController, 0, wxALL, 5);
		grid->Add(addRemove, wxGBPosition(4, 2), wxDefaultSpan, wxEXPAND, 5);

#ifdef HAS_BLUEZ
		auto* pair = new wxButton(tab, wxID_ANY, _("Pair Wii/Wii U Controller"));
		pair->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			PairingDialog dialog(this);
			dialog.ShowModal();
		});
		grid->Add(pair, wxGBPosition(5, 0), wxDefaultSpan, textFlags, 5);
#endif

		auto* actions = new wxBoxSizer(wxHORIZONTAL);
		page.settings = new wxButton(tab, wxID_ANY, _("Settings"));
		page.settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenControllerSettings(); });
		page.settings->Disable();
		actions->Add(page.settings, 0, wxALL, 5);

		page.calibrate = new wxButton(tab, wxID_ANY, _("Calibrate"));
		page.calibrate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			if (const auto controller = get_active_controller())
				controller->calibrate();
		});
		page.calibrate->Disable();
		actions->Add(page.calibrate, 0, wxALL, 5);

		page.clear = new wxButton(tab, wxID_ANY, _("Clear"));
		page.clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			// drops every mapping of the emulated controller
			ControllerPage& current = get_current_page_data();
			if (!current.emulated)
				return;
			if (InputPanel* panel = CurrentPanel())
				panel->reset_configuration();
			current.emulated->clear_mappings();
		});
		actions->Add(page.clear, 0, wxALL, 5);
		grid->Add(actions, wxGBPosition(5, 1), wxDefaultSpan, wxALIGN_CENTER_VERTICAL);

		page.connection = new wxBitmapButton(tab, wxID_ANY, m_disconnected);
		page.connection->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { TestConnection(); });
		page.connection->SetToolTip(_("Test if the controller is connected"));
		grid->Add(page.connection, wxGBPosition(5, 2), wxDefaultSpan, textFlags, 5);
	}
	addLine(6);

	// row 7: mapping panel of the emulated type
	page.panelArea = new wxBoxSizer(wxVERTICAL);
	grid->Add(page.panelArea, wxGBPosition(7, 0), wxGBSpan(1, 6), wxEXPAND | wxALL, 5);

	tab->SetSizer(grid);
	tab->Layout();
	tab->SetClientObject(new PageData(page));
	return tab;
}

// Brings the current tab in line with its emulated controller: type, list of
// physical controllers, button states and the mapping panel.
void InputSettings2::update_state()
{
	wxWindow* tab = m_notebook->GetCurrentPage();
	wxWindowUpdateLocker freeze(tab);
	ControllerPage& page = get_current_page_data();
	page.profileStatus->Hide();

	const EmulatedControllerPtr emulated = page.emulated;
	bool hasControllers = false;
	if (emulated)
	{
		const auto& devices = emulated->get_controllers();
		hasControllers = !devices.empty();
		page.emulatedType->SetSelection(page.emulatedType->Append(TypeName(emulated->type())));

		// rebuild the list, keeping the selection by name
		const wxString previous = page.controllers->GetStringSelection();
		page.controllers->Clear();
		for (const auto& device : devices)
			page.controllers->Append(fmt::format("{} [{}]", device->display_name(), device->api_name()), new ControllerItem(device));
		if (page.controllers->GetCount() > 0)
		{
			page.controllers->SetSelection(0);
			if (!previous.empty())
				page.controllers->SetStringSelection(previous);
		}
	}
	else
	{
		page.emulatedType->SetValue(_("Disabled"));
	}

	const ControllerPtr selected = SelectedController(page.controllers);
	page.connection->SetBitmap(selected && selected->is_connected() ? m_connected : m_disconnected);
	page.calibrate->Enable(hasControllers);
	page.removeController->Enable(hasControllers);
	page.settings->Enable(selected && HasSettingsDialog(selected->api()));

	// which panel is showing now
	std::optional<EmulatedController::Type> shown;
	for (int type = 0; type < EmulatedController::Type::MAX; ++type)
	{
		if (page.panels[type] && page.panels[type]->IsShown())
		{
			shown = (EmulatedController::Type)type;
			break;
		}
	}
	if (!shown && !emulated)
		return;
	if (shown && emulated && emulated->type() == *shown)
	{
		// right panel already up, refresh what it displays
		for (InputPanel* panel : page.panels)
		{
			if (panel)
				panel->load_controller(page.emulated);
		}
		return;
	}

	for (InputPanel* panel : page.panels)
	{
		if (panel)
			panel->Hide();
	}
	if (!emulated)
		return;

	const auto type = emulated->type();
	InputPanel*& panel = page.panels[type];
	if (!panel)
	{
		panel = CreatePanel(type, tab);
		if (!panel)
		{
			cemu_assert_debug(false);
			return;
		}
		page.panelArea->Add(panel, 0, wxEXPAND);
	}
	panel->load_controller(page.emulated);
	if (hasControllers)
		panel->set_selected_controller(emulated, emulated->get_controllers()[0]);
	panel->Show();
	tab->wxWindowBase::Layout();
	tab->wxWindow::Update();
}

void InputSettings2::OnControllersChanged()
{
	// re-test the connection of the selected controller on every built tab
	for (size_t i = 0; i < m_notebook->GetPageCount(); ++i)
	{
		wxWindow* tab = m_notebook->GetPage(i);
		const ControllerPage* page = tab ? PageOf(tab) : nullptr;
		if (!page)
			continue;
		if (const ControllerPtr controller = SelectedController(page->controllers))
			page->connection->SetBitmap(controller->connect() ? m_connected : m_disconnected);
	}
}

void InputSettings2::TestConnection()
{
	ControllerPage& page = get_current_page_data();
	if (const ControllerPtr controller = SelectedController(page.controllers))
		page.connection->SetBitmap(controller->connect() ? m_connected : m_disconnected);
}

// ---- profiles ----

void InputSettings2::FillProfileList(wxComboBox* profiles)
{
	wxWindowUpdateLocker freeze(profiles);
	const wxString selected = profiles->GetStringSelection();
	profiles->Clear();
	for (const auto& name : InputManager::get_profiles())
		profiles->Append(wxString::FromUTF8(name));
	profiles->SetStringSelection(selected);
}

void InputSettings2::OnProfileTextChanged(wxCommandEvent& event)
{
	auto* profiles = dynamic_cast<wxComboBox*>(event.GetEventObject());
	wxASSERT(profiles);
	ControllerPage& page = get_current_page_data();
	const wxString text = event.GetString();
	const bool known = profiles->FindString(text) != wxNOT_FOUND;
	page.profileLoad->Enable(known);
	page.profileSave->Enable(InputManager::is_valid_profilename(text.utf8_string()));
	page.profileDelete->Enable(known);
	page.profileStatus->Hide();
}

void InputSettings2::LoadProfile()
{
	ControllerPage& page = get_current_page_data();
	const std::string name = page.profiles->GetValue().utf8_string();
	page.profileStatus->Show();
	if (name.empty() || !InputManager::is_valid_profilename(name))
	{
		ShowStatus(page.profileStatus, _("invalid profile name"), false);
		page.profileStatus->Refresh();
		return;
	}
	const int slot = m_notebook->GetSelection();
	if (InputManager::instance().load(slot, name))
		ShowStatus(page.profileStatus, _("profile loaded"), true);
	else
		ShowStatus(page.profileStatus, _("couldn't load profile"), false);
	page.profileStatus->Refresh();

	// the profile may have replaced the emulated controller
	page.emulated = InputManager::instance().get_controller(slot);
	update_state();
}

void InputSettings2::SaveProfile()
{
	ControllerPage& page = get_current_page_data();
	const std::string name = page.profiles->GetValue().utf8_string();
	page.profileStatus->Show();
	if (name.empty() || !InputManager::is_valid_profilename(name))
		ShowStatus(page.profileStatus, _("invalid profile name"), false);
	else if (InputManager::instance().save(m_notebook->GetSelection(), name))
		ShowStatus(page.profileStatus, _("profile saved"), true);
	else
		ShowStatus(page.profileStatus, _("couldn't save profile"), false);
	page.profileStatus->Refresh();
}

void InputSettings2::DeleteProfile()
{
	ControllerPage& page = get_current_page_data();
	// only a profile picked from the list can be deleted
	const std::string name = page.profiles->GetStringSelection().utf8_string();
	page.profileStatus->Show();
	if (name.empty() || !InputManager::is_valid_profilename(name))
	{
		ShowStatus(page.profileStatus, _("invalid profile name"), false);
		page.profileStatus->Refresh();
		return;
	}
	try
	{
		// older versions stored profiles as .txt
		fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/{}.txt", name));
		fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/{}.xml", name));
		page.profiles->ChangeValue(kDefaultProfileName);
		ShowStatus(page.profileStatus, _("profile deleted"), true);
		page.profileLoad->Disable();
		page.profileSave->Disable();
		page.profileDelete->Disable();
	}
	catch (const std::exception&)
	{
		ShowStatus(page.profileStatus, _("can't delete profile"), false);
	}
	page.profileStatus->Refresh();
}

// ---- emulated controller ----

void InputSettings2::OnEmulatedTypeChosen(wxCommandEvent& event)
{
	const int slot = m_notebook->GetSelection();
	ControllerPage& page = get_current_page_data();

	if (event.GetSelection() == 0) // "Disabled"
	{
		page.emulated = {};
		InputManager::instance().delete_controller(slot, true);
		update_state();
		return;
	}

	try
	{
		const auto type = EmulatedController::type_from_string(event.GetString().utf8_string());
		if (page.emulated && page.emulated->type() == type)
			return;

		const auto created = InputManager::instance().set_controller(slot, type);
		page.emulated = created;
		// carry over controllers listed while the slot was disabled
		if (created->get_controllers().empty())
		{
			for (unsigned i = 0; i < page.controllers->GetCount(); ++i)
			{
				if (const ControllerPtr listed = ItemController(page.controllers, i))
					created->add_controller(listed);
			}
		}
		for (const auto& device : created->get_controllers())
			created->set_default_mapping(device);
	}
	catch (const std::exception&)
	{
		cemu_assert_debug(false);
	}
	update_state();
}

void InputSettings2::FillEmulatedTypeList(wxComboBox* types)
{
	wxWindowUpdateLocker freeze(types);

	// the slot's own type stays available even when its kind is at the limit
	const int selectedIndex = types->GetSelection();
	const wxString selected = types->GetStringSelection();
	const bool holdsGamepad = selectedIndex != wxNOT_FOUND && selected == TypeName(EmulatedController::Type::VPAD);
	const bool holdsWiiController = selectedIndex != wxNOT_FOUND && !holdsGamepad && selectedIndex != 0;
	const auto [gamepads, wiiControllers] = InputManager::instance().get_controller_count();

	types->Clear();
	types->AppendString(_("Disabled"));
	if (gamepads < InputManager::kMaxVPADControllers || holdsGamepad)
		types->Append(TypeName(EmulatedController::Type::VPAD));
	if (wiiControllers < InputManager::kMaxWPADControllers || holdsWiiController)
	{
		for (auto type : {EmulatedController::Type::Pro, EmulatedController::Type::Classic, EmulatedController::Type::Wiimote})
			types->AppendString(TypeName(type));
	}
	types->SetStringSelection(selected);
}

// ---- physical controllers ----

void InputSettings2::OnControllerChosen(wxCommandEvent& event)
{
	ControllerPage& page = get_current_page_data();
	const int index = event.GetSelection();
	page.removeController->Enable(index != wxNOT_FOUND);
	const ControllerPtr controller = ItemController(page.controllers, index);
	if (!controller)
		return;
	page.settings->Enable(HasSettingsDialog(controller->api()));
	if (InputPanel* panel = CurrentPanel())
		panel->set_selected_controller(page.emulated, controller);
}

void InputSettings2::AddController()
{
	ControllerPage& page = get_current_page_data();

	std::vector<ControllerPtr> listed;
	listed.reserve(page.controllers->GetCount());
	for (unsigned i = 0; i < page.controllers->GetCount(); ++i)
	{
		if (ControllerPtr controller = ItemController(page.controllers, i))
			listed.emplace_back(std::move(controller));
	}

	InputAPIAddWindow picker(this, wxGetMousePosition() + wxSize(5, 5), listed);
	if (picker.ShowModal() != wxID_OK)
		return;
	wxASSERT(picker.is_valid());

	const ControllerPtr controller = picker.get_controller();
	controller->connect();
	const int index = page.controllers->Append(fmt::format("{} [{}]", controller->display_name(), to_string(picker.get_type())), new ControllerItem(controller));
	page.controllers->Select(index);

	if (page.emulated)
	{
		page.emulated->add_controller(controller);
		InputPanel* panel = CurrentPanel();
		// the first controller of an unmapped emulated controller gets the default mapping
		if (page.emulated->set_default_mapping(controller) && panel)
			panel->load_controller(page.emulated);
		if (panel)
			panel->set_selected_controller(page.emulated, controller);
	}
	update_state();
}

void InputSettings2::RemoveController()
{
	ControllerPage& page = get_current_page_data();
	wxComboBox* list = page.controllers;
	int index = list->GetSelection();
	if (index == wxNOT_FOUND)
		return;

	if (page.emulated)
	{
		InputPanel* panel = CurrentPanel();
		if (const ControllerPtr controller = ItemController(list, index))
		{
			page.emulated->remove_controller(controller);
			if (panel)
				panel->load_controller(page.emulated);
		}
		if (panel)
			panel->reset_colours();
	}

	list->Delete(index);
	list->Refresh();
	update_state();
	// select the neighbour of the removed entry
	if (list->GetCount() > 0)
		list->SetSelection(index > 0 ? index - 1 : 0);
	update_state();
}

void InputSettings2::OpenControllerSettings()
{
	const ControllerPtr controller = get_active_controller();
	if (!controller)
		return;
	const wxPoint where = wxGetMousePosition() + wxSize(5, 5);
	switch (controller->api())
	{
	case InputAPI::DirectInput:
	case InputAPI::XInput:
	case InputAPI::GameCube:
	case InputAPI::WGIGamepad:
	case InputAPI::WGIRawController:
	case InputAPI::SDLController:
	case InputAPI::DSUClient:
	{
		DefaultControllerSettings dialog(this, where, controller);
		dialog.ShowModal();
		break;
	}
#ifdef SUPPORTS_WIIMOTE
	case InputAPI::Wiimote:
	{
		const auto wiimote = std::dynamic_pointer_cast<NativeWiimoteController>(controller);
		wxASSERT(wiimote);
		WiimoteControllerSettings dialog(this, where, wiimote);
		dialog.ShowModal();
		break;
	}
#endif
	default:
		break;
	}
}
