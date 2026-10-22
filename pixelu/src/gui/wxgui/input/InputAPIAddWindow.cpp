// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/input/InputAPIAddWindow.h"

#include "input/InputManager.h"
#include "input/ControllerFactory.h"
#include "input/api/Controller.h"
#include "wxgui/helpers/wxCustomData.h"
#include "wxgui/helpers/wxHelpers.h"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wupdlock.h>

namespace
{
	using ControllerItem = wxCustomData<ControllerPtr>;

	bool IsApiNamed(const wxString& shown, InputAPI::Type api)
	{
		return shown == wxString::FromUTF8(to_string(api));
	}
}

InputAPIAddWindow::InputAPIAddWindow(wxWindow* parent, const wxPoint& position, const std::vector<ControllerPtr>& controllers)
	: wxDialog(parent, wxID_ANY, "Add input API", position, wxDefaultSize, wxCAPTION), m_controllers(controllers)
{
	SetSizeHints(wxDefaultSize, wxDefaultSize);
	auto* layout = new wxBoxSizer(wxVERTICAL);

	// API / controller pickers
	{
		auto* grid = new wxFlexGridSizer(2);

		grid->Add(new wxStaticText(this, wxID_ANY, _("API")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		m_apiChoice = new wxChoice(this, wxID_ANY);
		// one entry per API that has a provider
		for (const auto& providers : InputManager::instance().get_api_providers())
		{
			if (providers.empty())
				continue;
			const auto& provider = *providers.begin();
			m_apiChoice->Append(wxString::FromUTF8(provider->api_name()), new wxAPIType(provider->api()));
		}
		m_apiChoice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { OnApiChosen(); });
		grid->Add(m_apiChoice, 1, wxALL | wxEXPAND, 5);

		grid->Add(new wxStaticText(this, wxID_ANY, _("Controller")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		m_controllerChoice = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
		m_controllerChoice->Bind(wxEVT_COMBOBOX_DROPDOWN, [this](wxCommandEvent&) { StartSearch(); });
		m_controllerChoice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { OnControllerChosen(); });
		m_controllerChoice->SetMinSize(wxSize(240, -1));
		m_controllerChoice->Disable();
		grid->Add(m_controllerChoice, 1, wxALL | wxEXPAND, 5);

		layout->Add(grid, 0, wxEXPAND, 5);
	}

	layout->Add(new wxStaticLine(this), 0, wxEXPAND);

	{
		auto* buttons = new wxBoxSizer(wxHORIZONTAL);
		m_addButton = new wxButton(this, wxID_ANY, _("Add"));
		m_addButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnAdd(); });
		m_addButton->Disable();
		buttons->Add(m_addButton, 0, wxALL, 5);

		auto* cancel = new wxButton(this, wxID_ANY, _("Cancel"));
		cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
		buttons->Add(cancel, 0, wxALL, 5);
		layout->Add(buttons, 0, wxEXPAND, 5);
	}

	// DSU host/port, only shown while DSU client is picked
	{
		m_dsuPanel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
		auto* panelLayout = new wxBoxSizer(wxVERTICAL);
		panelLayout->Add(new wxStaticLine(m_dsuPanel), 0, wxEXPAND, 0);

		auto* fields = new wxBoxSizer(wxHORIZONTAL);
		fields->Add(new wxStaticText(m_dsuPanel, wxID_ANY, _("IP")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		m_ip = new wxTextCtrl(m_dsuPanel, wxID_ANY, "127.0.0.1");
		fields->Add(m_ip, 0, wxALL, 5);
		fields->Add(new wxStaticText(m_dsuPanel, wxID_ANY, _("Port")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
		m_port = new wxTextCtrl(m_dsuPanel, wxID_ANY, "26760");
		fields->Add(m_port, 0, wxALL, 5);
		panelLayout->Add(fields, 0, wxEXPAND);

		m_dsuPanel->SetSizer(panelLayout);
		m_dsuPanel->Layout();
		m_dsuPanel->Hide();
		layout->Add(m_dsuPanel, 1, wxEXPAND);
	}

	SetSizer(layout);
	Layout();
	layout->Fit(this);
}

InputAPIAddWindow::~InputAPIAddWindow()
{
	CancelSearch();
}

std::optional<InputAPI::Type> InputAPIAddWindow::SelectedApi() const
{
	const int index = m_apiChoice->GetSelection();
	if (index == wxNOT_FOUND)
		return std::nullopt;
	return static_cast<wxAPIType*>(m_apiChoice->GetClientObject(index))->get();
}

bool InputAPIAddWindow::has_custom_settings() const
{
	return IsApiNamed(m_apiChoice->GetStringSelection(), InputAPI::DSUClient);
}

std::unique_ptr<ControllerProviderSettings> InputAPIAddWindow::get_settings() const
{
	if (!has_custom_settings())
		return {};
	return std::make_unique<DSUProviderSettings>(m_ip->GetValue().ToStdString(), ConvertString<uint16>(m_port->GetValue().ToStdString()));
}

void InputAPIAddWindow::OnAdd()
{
	const auto api = SelectedApi();
	if (!api)
	{
		cemu_assert_debug(false);
		EndModal(wxID_CANCEL);
		return;
	}
	const bool duplicate = std::any_of(m_controllers.begin(), m_controllers.end(),
									   [this](const ControllerPtr& c) { return *c == *m_controller; });
	if (duplicate)
	{
		wxMessageBox(_("The controller is already added!"), _("Error"), wxOK | wxCENTRE | wxICON_ERROR, this);
		return;
	}
	m_type = *api;
	EndModal(wxID_OK);
}

void InputAPIAddWindow::OnApiChosen()
{
	CancelSearch();
	if (m_apiChoice->GetSelection() == wxNOT_FOUND)
		return;

	// a controller picked for the previous API does not carry over
	m_controller.reset();
	m_addButton->Disable();
	m_controllerChoice->Enable();
	m_controllerChoice->SetSelection(wxNOT_FOUND);

	if (IsApiNamed(m_apiChoice->GetStringSelection(), InputAPI::Keyboard))
	{
		// there is only ever the one keyboard, pick it right away
		const auto keyboards = InputManager::instance().get_api_provider(InputAPI::Keyboard)->get_controllers();
		if (!keyboards.empty())
		{
			m_controller = keyboards.front();
			m_addButton->Enable();
			m_controllerChoice->Clear();
			const int index = m_controllerChoice->Append(keyboards.front()->display_name(), new ControllerItem(keyboards.front()));
			m_controllerChoice->SetSelection(index);
		}
	}
	else
	{
#if BOOST_OS_LINUX || BOOST_OS_BSD
		// wxGTK will not open the dropdown of an empty combo box, so the
		// dropdown event would never fire; search immediately instead
		StartSearch();
#endif
	}

	const bool wantDsu = has_custom_settings();
	if (wantDsu != m_dsuPanel->IsShown())
	{
		wxWindowUpdateLocker freeze(this);
		m_dsuPanel->Show(wantDsu);
		Layout();
		Fit();
	}
}

void InputAPIAddWindow::StartSearch()
{
	if (m_searching)
		return;
	const auto api = SelectedApi();
	if (!api)
		return;

	ControllerProviderPtr provider;
	if (const auto settings = get_settings())
		provider = InputManager::instance().get_api_provider(*api, *settings);
	else
		provider = InputManager::instance().get_api_provider(*api);
	if (!provider)
		return;

	m_searching = true;
	{
		wxWindowUpdateLocker freeze(m_controllerChoice);
		m_controllerChoice->Clear();
		m_controllerChoice->Append(_("Searching for controllers..."), (wxClientData*)nullptr);
		m_controllerChoice->SetSelection(wxNOT_FOUND);
	}

	auto ticket = std::make_shared<SearchTicket>();
	m_search = ticket;
	std::thread([this, provider, ticket]() {
		auto found = provider->get_controllers();
		std::lock_guard lock(ticket->mutex);
		// a cancelled search may outlive the dialog, so it must not touch it
		if (ticket->cancelled)
			return;
		CallAfter([this, found = std::move(found)]() { FillControllers(found); });
		m_searching = false;
	}).detach();
}

void InputAPIAddWindow::FillControllers(const std::vector<ControllerPtr>& found)
{
	wxWindowUpdateLocker freeze(m_controllerChoice);
	m_controllerChoice->Clear();
	for (const auto& controller : found)
		m_controllerChoice->Append(controller->display_name(), new ControllerItem(controller));
}

void InputAPIAddWindow::OnControllerChosen()
{
	if (m_searching)
		return;
	const int index = m_controllerChoice->GetSelection();
	if (index == wxNOT_FOUND)
		return;
	// the "searching" placeholder has no data
	if (auto* item = static_cast<ControllerItem*>(m_controllerChoice->GetClientObject(index)))
	{
		m_controller = item->ref();
		m_addButton->Enable();
	}
}

void InputAPIAddWindow::CancelSearch()
{
	m_searching = false;
	if (m_search)
	{
		std::lock_guard lock(m_search->mutex);
		m_search->cancelled = true;
	}
}
