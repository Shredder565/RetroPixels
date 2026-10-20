// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "input/api/InputAPI.h"

#include <optional>
#include <wx/dialog.h>
#include <wx/panel.h>

#include "wxgui/helpers/wxCustomData.h"
#include "input/api/Controller.h"

class wxButton;
class wxComboBox;
class wxChoice;
class wxTextCtrl;

using wxAPIType = wxCustomData<InputAPI::Type>;

// Picks an input API and one of its controllers to add to a profile.
// Controller lists are fetched on a worker thread when the list is opened.
class InputAPIAddWindow : public wxDialog
{
public:
	InputAPIAddWindow(wxWindow* parent, const wxPoint& position, const std::vector<ControllerPtr>& controllers);
	~InputAPIAddWindow();

	bool is_valid() const { return m_type.has_value() && m_controller != nullptr; }
	InputAPI::Type get_type() const { return m_type.value(); }
	std::shared_ptr<ControllerBase> get_controller() const { return m_controller; }

	// DSU client needs a host and port
	bool has_custom_settings() const;
	std::unique_ptr<ControllerProviderSettings> get_settings() const;

private:
	struct SearchTicket
	{
		std::mutex mutex;
		bool cancelled = false;
	};

	std::optional<InputAPI::Type> SelectedApi() const;
	void OnAdd();
	void OnApiChosen();
	void StartSearch();
	void OnControllerChosen();
	void FillControllers(const std::vector<ControllerPtr>& found);
	void CancelSearch();

	wxChoice* m_apiChoice;
	wxComboBox* m_controllerChoice;
	wxButton* m_addButton;

	wxPanel* m_dsuPanel;
	wxTextCtrl* m_ip;
	wxTextCtrl* m_port;

	std::optional<InputAPI::Type> m_type;
	std::shared_ptr<ControllerBase> m_controller;

	std::vector<ControllerPtr> m_controllers; // already in the profile
	std::atomic_bool m_searching = false;
	std::shared_ptr<SearchTicket> m_search;
};
