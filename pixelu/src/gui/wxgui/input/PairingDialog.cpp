// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "wxgui/wxgui.h"
#include "PairingDialog.h"

#if BOOST_OS_WINDOWS
#include <bluetoothapis.h>
#endif
#if BOOST_OS_LINUX
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <input/api/Wiimote/l2cap/L2CapWiimote.h>
#endif

namespace
{
	// carries a PairingState from the worker to the dialog
	wxDEFINE_EVENT(EVT_PAIRING_STATE, wxCommandEvent);
}

PairingDialog::PairingDialog(wxWindow* parent)
	: wxDialog(parent, wxID_ANY, _("Pairing..."), wxDefaultPosition, wxDefaultSize,
			   wxCAPTION | wxMINIMIZE_BOX | wxSYSTEM_MENU | wxTAB_TRAVERSAL | wxCLOSE_BOX)
{
	auto* layout = new wxBoxSizer(wxVERTICAL);

	m_progress = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(350, 20), wxGA_HORIZONTAL);
	m_progress->SetValue(0);
	layout->Add(m_progress, 0, wxALL | wxEXPAND, 5);

	auto* bottom = new wxFlexGridSizer(0, 2, 0, 0);
	bottom->AddGrowableCol(1);
	m_status = new wxStaticText(this, wxID_ANY, _("Searching for controllers..."));
	bottom->Add(m_status, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

	auto* buttonBox = new wxBoxSizer(wxHORIZONTAL);
	m_cancel = new wxButton(this, wxID_ANY, _("Cancel"));
	m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
	buttonBox->Add(m_cancel, 0, wxALL, 5);
	bottom->Add(buttonBox, 1, wxALIGN_RIGHT, 5);
	layout->Add(bottom, 0, wxALL | wxEXPAND, 5);

	SetSizerAndFit(layout);
	Centre(wxBOTH);

	Bind(wxEVT_CLOSE_WINDOW, &PairingDialog::OnCloseWindow, this);
	Bind(EVT_PAIRING_STATE, &PairingDialog::OnStateEvent, this);

	m_worker = std::thread(&PairingDialog::Search, this);
}

PairingDialog::~PairingDialog()
{
	Unbind(wxEVT_CLOSE_WINDOW, &PairingDialog::OnCloseWindow, this);
	// normally already joined by the close handler
	m_stop = true;
	if (m_worker.joinable())
		m_worker.join();
}

void PairingDialog::OnCloseWindow(wxCloseEvent& event)
{
	event.Skip();
	m_stop = true;
	if (m_worker.joinable())
		m_worker.join();
}

void PairingDialog::Report(PairingState state)
{
	auto* event = new wxCommandEvent(EVT_PAIRING_STATE);
	event->SetInt((int)state);
	wxQueueEvent(this, event);
}

void PairingDialog::OnStateEvent(wxCommandEvent& event)
{
	ShowState((PairingState)event.GetInt());
}

void PairingDialog::ShowState(PairingState state)
{
	wxString text;
	int progress = 0;
	switch (state)
	{
	case PairingState::Pairing:
		// still working, keep the Cancel button
		m_status->SetLabel(_("Found controller. Pairing..."));
		m_progress->SetValue(50);
		return;
	case PairingState::Finished:
		text = _("Successfully paired the controller.");
		progress = 100;
		break;
	case PairingState::NoBluetoothAvailable:
		text = _("Failed to find a suitable Bluetooth radio.");
		break;
	case PairingState::SearchFailed:
		text = _("Failed to find controllers.");
		break;
	case PairingState::PairingFailed:
		text = _("Failed to pair with the found controller.");
		break;
	case PairingState::BluetoothUnusable:
		text = _("Please use your system's Bluetooth manager instead.");
		break;
	default:
		return;
	}
	// every other state ends the search
	m_status->SetLabel(text);
	m_progress->SetValue(progress);
	m_cancel->SetLabel(_("Close"));
}

#if BOOST_OS_WINDOWS

void PairingDialog::Search()
{
	constexpr std::wstring_view kWiimote = L"Nintendo RVL-CNT-01";
	constexpr std::wstring_view kProController = L"Nintendo RVL-CNT-01-UC";
	// Bluetooth HID service class
	const GUID hidService = {0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

	BLUETOOTH_FIND_RADIO_PARAMS radioQuery{};
	radioQuery.dwSize = sizeof(radioQuery);
	HANDLE radio = INVALID_HANDLE_VALUE;
	HBLUETOOTH_RADIO_FIND radioFind = BluetoothFindFirstRadio(&radioQuery, &radio);
	if (!radioFind)
	{
		Report(PairingState::NoBluetoothAvailable);
		return;
	}
	BluetoothFindRadioClose(radioFind);
	stdx::scope_exit closeRadio([radio]() { CloseHandle(radio); });

	BLUETOOTH_RADIO_INFO radioInfo{};
	radioInfo.dwSize = sizeof(radioInfo);
	if (BluetoothGetRadioInfo(radio, &radioInfo) != ERROR_SUCCESS)
	{
		Report(PairingState::NoBluetoothAvailable);
		return;
	}

	BLUETOOTH_DEVICE_SEARCH_PARAMS query{};
	query.dwSize = sizeof(query);
	query.fReturnAuthenticated = FALSE;
	query.fReturnRemembered = FALSE;
	query.fReturnUnknown = TRUE;
	query.fReturnConnected = FALSE;
	query.fIssueInquiry = TRUE;
	query.cTimeoutMultiplier = 5;
	query.hRadio = radio;

	BLUETOOTH_DEVICE_INFO device{};
	device.dwSize = sizeof(device);

	// keep inquiring until a controller shows up or the dialog closes
	while (!m_stop)
	{
		HBLUETOOTH_DEVICE_FIND found = BluetoothFindFirstDevice(&query, &device);
		if (!found)
		{
			Report(PairingState::SearchFailed);
			return;
		}
		bool more = true;
		while (more && !m_stop)
		{
			const std::wstring_view name = device.szName;
			if (name != kWiimote && name != kProController)
			{
				more = BluetoothFindNextDevice(found, &device) != FALSE;
				continue;
			}
			BluetoothFindDeviceClose(found);
			Report(PairingState::Pairing);

			// the remote expects the host's radio address as the PIN, one byte per character
			wchar_t pin[6];
			for (int i = 0; i < 6; i++)
				pin[i] = radioInfo.address.rgBytes[i];
			if (BluetoothAuthenticateDevice(nullptr, radio, &device, pin, 6) != ERROR_SUCCESS ||
				BluetoothSetServiceState(radio, &device, &hidService, BLUETOOTH_SERVICE_ENABLE) != ERROR_SUCCESS)
			{
				Report(PairingState::PairingFailed);
				return;
			}
			Report(PairingState::Finished);
			return;
		}
		BluetoothFindDeviceClose(found);
	}
}

#elif defined(HAS_BLUEZ)

void PairingDialog::Search()
{
	// limited inquiry access code, used by remotes in sync mode
	static constexpr uint8_t kLimitedInquiryLap[] = {0x00, 0x8b, 0x9e};
	auto isRemoteName = [](std::string_view name) {
		return name == "Nintendo RVL-CNT-01" || name == "Nintendo RVL-CNT-01-TR";
	};

	const int adapter = hci_get_route(nullptr);
	if (adapter < 0)
	{
		Report(PairingState::NoBluetoothAvailable);
		return;
	}

	// the inquiry blocks for several seconds and cannot be interrupted
	CallAfter([this]() { m_cancel->Disable(); });
	inquiry_info* replies = nullptr;
	const int replyCount = hci_inquiry(adapter, 7, 4, kLimitedInquiryLap, &replies, IREQ_CACHE_FLUSH);
	CallAfter([this]() { m_cancel->Enable(); });
	if (replyCount <= 0)
	{
		Report(PairingState::SearchFailed);
		return;
	}
	stdx::scope_exit freeReplies([&]() { bt_free(replies); });
	if (m_stop)
		return;

	const int socket = hci_open_dev(adapter);
	stdx::scope_exit closeSocket([&]() { hci_close_dev(socket); });

	// match by name: many third-party remotes carry no usable SDP vendor/product ids
	bool anyRemote = false;
	char name[HCI_MAX_NAME_LENGTH] = {};
	for (const inquiry_info& reply : std::span(replies, replyCount))
	{
		const bool failed = hci_read_remote_name(socket, &reply.bdaddr, HCI_MAX_NAME_LENGTH, name, 2000) != 0;
		if (m_stop)
			return;
		if (failed || !isRemoteName(name))
			continue;
		L2CapWiimote::AddCandidateAddress(reply.bdaddr);
		anyRemote = true;
		const uint8_t* a = reply.bdaddr.b;
		cemuLog_log(LogType::Force, "Pairing Dialog: Found '{}' with address '{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}'",
					name, a[5], a[4], a[3], a[2], a[1], a[0]);
	}
	Report(anyRemote ? PairingState::Finished : PairingState::SearchFailed);
}

#else

void PairingDialog::Search()
{
	Report(PairingState::BluetoothUnusable);
}

#endif
