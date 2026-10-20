// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteReal/IOWin.h"

#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <bluetoothapis.h>
#include <cfgmgr32.h>
#include <hidclass.h>
#include <hidsdi.h>
#include <initguid.h>
#include <wtypes.h>
// devpkey.h needs initguid.h first, propkey.h needs wtypes.h first.
#include <devpkey.h>
#include <propkey.h>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"

#include "Core/HW/WiimoteCommon/DataReport.h"
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "Core/HW/WiimoteCommon/WiimoteReport.h"

#pragma comment(lib, "Bthprops.lib")

// Two halves: Bluetooth housekeeping through the Windows Bluetooth API (pairing, enabling the HID
// service, forgetting stale pairings), and talking to remotes through the HID interfaces that
// Windows (or a DolphinBar) then exposes.

namespace WiimoteReal
{
namespace
{
using WiimoteCommon::InputReportID;
using WiimoteCommon::OutputReportID;

// Bluetooth inquiry length in units of 1.28 s.
constexpr u8 INQUIRY_LENGTH = 3;

// Whether a device walk goes on to the next item.
enum class Walk : bool
{
  Stop,
  Continue,
};

// The PIN a remote expects: the host's address after the red sync button, its own after 1+2.
enum class PairingKey : bool
{
  RemoteAddress,
  HostAddress,
};

// Calls `visit(radio)` for each local Bluetooth radio. Each radio handle is closed after the
// next one has been found.
template <typename Visitor>
void ForEachRadio(Visitor&& visit)
{
  BLUETOOTH_FIND_RADIO_PARAMS params{};
  params.dwSize = static_cast<DWORD>(sizeof(params));

  HANDLE radio = nullptr;
  const HBLUETOOTH_RADIO_FIND search = BluetoothFindFirstRadio(&params, &radio);
  if (search == nullptr)
  {
    ERROR_LOG_FMT(WIIMOTE, "No Bluetooth radio found: {}", Common::GetLastErrorString());
    return;
  }

  for (;;)
  {
    const HANDLE current = radio;
    const bool more = visit(current) == Walk::Continue && BluetoothFindNextRadio(search, &radio);
    CloseHandle(current);
    if (!more)
      break;
  }

  BluetoothFindRadioClose(search);
}

// Calls `visit(radio, radio_info, device)` for the Bluetooth devices each radio knows of, after
// an inquiry of `inquiry_length` (0 lists known devices only). Returning Stop ends the walk over
// that radio's devices.
template <typename Visitor>
void ForEachBluetoothDevice(u8 inquiry_length, Visitor&& visit)
{
  BLUETOOTH_DEVICE_SEARCH_PARAMS search_params{};
  search_params.dwSize = static_cast<DWORD>(sizeof(search_params));
  search_params.fReturnAuthenticated = TRUE;
  search_params.fReturnRemembered = TRUE;
  search_params.fReturnUnknown = TRUE;
  search_params.fReturnConnected = TRUE;
  search_params.fIssueInquiry = inquiry_length != 0 ? TRUE : FALSE;
  search_params.cTimeoutMultiplier = inquiry_length;

  ForEachRadio([&](HANDLE radio) {
    BLUETOOTH_RADIO_INFO radio_info{};
    radio_info.dwSize = static_cast<DWORD>(sizeof(radio_info));
    if (BluetoothGetRadioInfo(radio, &radio_info) != ERROR_SUCCESS)
    {
      ERROR_LOG_FMT(WIIMOTE, "Could not query a Bluetooth radio");
      return Walk::Continue;
    }

    search_params.hRadio = radio;

    BLUETOOTH_DEVICE_INFO device{};
    device.dwSize = static_cast<DWORD>(sizeof(device));
    const HBLUETOOTH_DEVICE_FIND search = BluetoothFindFirstDevice(&search_params, &device);
    if (search == nullptr)
    {
      const DWORD error = GetLastError();
      if (error != ERROR_NO_MORE_ITEMS)
      {
        ERROR_LOG_FMT(WIIMOTE, "Bluetooth device search failed: {}",
                      Common::GetWin32ErrorString(error));
      }
      return Walk::Continue;
    }

    while (visit(radio, std::as_const(radio_info), device) == Walk::Continue &&
           BluetoothFindNextDevice(search, &device))
    {
    }

    BluetoothFindDeviceClose(search);
    return Walk::Continue;
  });
}

// Like ForEachBluetoothDevice, limited to devices with a Wii Remote or balance board name.
template <typename Visitor>
void ForEachBluetoothWiimote(u8 inquiry_length, Visitor&& visit)
{
  ForEachBluetoothDevice(inquiry_length, [&](HANDLE radio, const BLUETOOTH_RADIO_INFO& radio_info,
                                             BLUETOOTH_DEVICE_INFO& device) {
    if (!IsValidDeviceName(WStringToUTF8(device.szName)))
      return Walk::Continue;
    return visit(radio, radio_info, device);
  });
}

// BluetoothAuthenticateDevice is deprecated, but its replacement needs a callback setup that
// would buy nothing here.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4995)
#endif
bool Authenticate(HANDLE radio, const BLUETOOTH_RADIO_INFO& radio_info,
                  BLUETOOTH_DEVICE_INFO& device, PairingKey key)
{
  // There is no telling which PIN the remote wants, and a wrong one makes Windows drop it.
  const BLUETOOTH_ADDRESS& address =
      key == PairingKey::HostAddress ? radio_info.address : device.Address;

  // The PIN is the address bytes as stored (least significant first), one character each.
  std::array<WCHAR, sizeof(address.rgBytes)> pin{};
  std::ranges::copy(address.rgBytes, pin.begin());

  const DWORD result =
      BluetoothAuthenticateDevice(nullptr, radio, &device, pin.data(), ULONG(pin.size()));
  if (result != ERROR_SUCCESS)
  {
    // Typically ERROR_NO_MORE_ITEMS or ERROR_GEN_FAILURE.
    ERROR_LOG_FMT(WIIMOTE, "Bluetooth authentication failed: {}", result);
    return false;
  }

  // Without this the remote does not keep the pairing.
  DWORD service_count = 0;
  const DWORD services_result =
      BluetoothEnumerateInstalledServices(radio, &device, &service_count, nullptr);
  if (services_result != ERROR_SUCCESS && services_result != ERROR_MORE_DATA)
  {
    ERROR_LOG_FMT(WIIMOTE, "Listing the remote's Bluetooth services failed: {}", services_result);
    return false;
  }

  return true;
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// Unpairs the known remotes that `should_remove` picks and returns how many were removed.
template <typename Predicate>
u32 RemoveWiimotes(Predicate&& should_remove)
{
  u32 removed = 0;
  ForEachBluetoothWiimote(0, [&](HANDLE, const BLUETOOTH_RADIO_INFO&,
                                 BLUETOOTH_DEVICE_INFO& device) {
    if (!should_remove(std::as_const(device)))
      return Walk::Continue;

    NOTICE_LOG_FMT(WIIMOTE, "Removing a paired Wii Remote");
    const DWORD result = BluetoothRemoveDevice(&device.Address);
    if (result != ERROR_SUCCESS)
      ERROR_LOG_FMT(WIIMOTE, "Removing the Bluetooth device failed: {}", result);
    else
      ++removed;
    return Walk::Continue;
  });
  return removed;
}

// A remembered remote that is neither connected nor authenticated can never reconnect (an
// authenticated one reconnects at a button press), so it is removed to let it pair again rather
// than making the user delete it in the control panel.
u32 RemoveUnusableWiimotes()
{
  return RemoveWiimotes([](const BLUETOOTH_DEVICE_INFO& device) {
    return device.fRemembered && !device.fConnected && !device.fAuthenticated;
  });
}

// Enables the HID service of each unconnected remote found, authenticating it first when a key
// is given. Returns how many succeeded.
u32 PairWiimotes(u8 inquiry_length, std::optional<PairingKey> key = std::nullopt)
{
  u32 paired = 0;
  ForEachBluetoothWiimote(inquiry_length, [&](HANDLE radio, const BLUETOOTH_RADIO_INFO& radio_info,
                                              BLUETOOTH_DEVICE_INFO& device) {
    if (device.fConnected)
      return Walk::Continue;

    INFO_LOG_FMT(WIIMOTE, "Found Bluetooth device {}", WStringToUTF8(device.szName));

    if (key.has_value() && !device.fAuthenticated)
    {
      INFO_LOG_FMT(WIIMOTE, "Authenticating the Wii Remote");
      Authenticate(radio, radio_info, device, *key);
    }

    INFO_LOG_FMT(WIIMOTE, "Enabling the Wii Remote's HID service");
    const DWORD result = BluetoothSetServiceState(
        radio, &device, &HumanInterfaceDeviceServiceClass_UUID, BLUETOOTH_SERVICE_ENABLE);
    if (result != ERROR_SUCCESS)
    {
      // Typically ERROR_INVALID_PARAMETER.
      ERROR_LOG_FMT(WIIMOTE, "Enabling the HID service failed: {}", result);
      return Walk::Continue;
    }

    ++paired;
    return Walk::Continue;
  });
  return paired;
}

// Vendor and product id of a HID interface. It is opened without read or write access, which is
// enough for the attributes.
std::optional<USBUtils::DeviceInfo> ReadDeviceIds(const WCHAR* hid_iface)
{
  const HANDLE handle = CreateFile(hid_iface, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
  {
    WARN_LOG_FMT(WIIMOTE, "Could not open a HID interface for its ids");
    return std::nullopt;
  }

  HIDD_ATTRIBUTES attributes{};
  attributes.Size = static_cast<ULONG>(sizeof(attributes));
  const bool have_attributes = HidD_GetAttributes(handle, &attributes) != FALSE;
  CloseHandle(handle);

  if (!have_attributes)
  {
    ERROR_LOG_FMT(WIIMOTE, "Could not read the attributes of a HID interface");
    return std::nullopt;
  }
  return USBUtils::DeviceInfo{attributes.VendorID, attributes.ProductID};
}

// The device node an interface belongs to.
std::optional<DEVINST> DevNodeOf(const WCHAR* hid_iface)
{
  std::optional<std::wstring> instance_id =
      Common::GetDeviceInterfaceStringProperty(hid_iface, &DEVPKEY_Device_InstanceId);
  if (!instance_id.has_value())
    return std::nullopt;

  DEVINST node{};
  if (CM_Locate_DevNode(&node, instance_id->data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
  {
    ERROR_LOG_FMT(WIIMOTE, "Could not locate the device node of a HID interface");
    return std::nullopt;
  }
  return node;
}

std::optional<DEVINST> ParentOf(DEVINST node)
{
  DEVINST parent{};
  if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS)
  {
    ERROR_LOG_FMT(WIIMOTE, "Could not find the parent of a device node");
    return std::nullopt;
  }
  return parent;
}

std::optional<std::wstring> BusReportedDescriptionOf(DEVINST node)
{
  return Common::GetDevNodeStringProperty(node, &DEVPKEY_Device_BusReportedDeviceDesc);
}

// The Bluetooth name ("Nintendo RVL-CNT-01", "Nintendo RVL-WBC-01") of the device behind a HID
// device node: the sibling of class "Bluetooth" with the same association endpoint id (a string
// like "Bluetooth#Bluetooth<radio address>-<device address>").
std::optional<std::string> BluetoothNameOf(DEVINST node)
{
  const auto aep_key = std::bit_cast<DEVPROPKEY>(PKEY_Devices_Aep_AepId);

  const std::optional<std::wstring> aep_id = Common::GetDevNodeStringProperty(node, &aep_key);
  if (!aep_id.has_value())
    return std::nullopt;

  const std::optional<DEVINST> parent = ParentOf(node);
  if (!parent.has_value())
    return std::nullopt;

  DEVINST sibling{};
  for (CONFIGRET status = CM_Get_Child(&sibling, *parent, 0); status == CR_SUCCESS;
       status = CM_Get_Sibling(&sibling, sibling, 0))
  {
    const bool is_bluetooth =
        Common::GetDevNodeStringProperty(sibling, &DEVPKEY_Device_Class) == L"Bluetooth";
    if (is_bluetooth && *aep_id == Common::GetDevNodeStringProperty(sibling, &aep_key))
      return Common::GetDevNodeStringProperty(sibling, &DEVPKEY_NAME).transform(WStringToUTF8);
  }
  return std::nullopt;
}

// HID reads on Windows always fill the whole buffer, so the real length comes from the report
// id (sizes as on WiiBrew). 0 for an unknown id.
size_t InputReportSize(u8 report_id)
{
  const auto id = static_cast<InputReportID>(report_id);
  switch (id)
  {
  case InputReportID::Status:
    return sizeof(WiimoteCommon::InputReportStatus);
  case InputReportID::ReadDataReply:
    return sizeof(WiimoteCommon::InputReportReadDataReply);
  case InputReportID::Ack:
    return sizeof(WiimoteCommon::InputReportAck);
  default:
    break;
  }

  if (!WiimoteCommon::DataReportBuilder::IsValidMode(id))
    return 0;
  return WiimoteCommon::MakeDataReportManipulator(id, nullptr)->GetDataSize();
}

// Every present HID interface that looks like a Wii device, by Bluetooth name, by the bus
// reported description, or failing those by USB ids.
std::vector<WiimoteScannerWindows::EnumeratedWiimoteInterface> ListWiimoteInterfaces()
{
  // Every DolphinBar revision reports this description (and 057e:0306), says Mayflash.
  constexpr std::string_view DOLPHINBAR_DESCRIPTION = "Mayflash Wiimote PC Adapter";

  std::vector<WiimoteScannerWindows::EnumeratedWiimoteInterface> found;

  GUID hid_class = GUID_DEVINTERFACE_HID;
  for (auto* hid_iface :
       Common::GetDeviceInterfaceList(&hid_class, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT))
  {
    DEBUG_LOG_FMT(WIIMOTE, "Checking a HID interface");

    const std::optional<DEVINST> parent = DevNodeOf(hid_iface).and_then(ParentOf);

    const std::optional<std::string> bluetooth_name = parent.and_then(BluetoothNameOf);
    DEBUG_LOG_FMT(WIIMOTE, "  Bluetooth name: {}", bluetooth_name.value_or("<error>"));

    // Balance boards report "Nintendo RVL-CNT-01" here.
    const std::optional<std::string> description =
        parent.and_then(BusReportedDescriptionOf).transform(WStringToUTF8);
    DEBUG_LOG_FMT(WIIMOTE, "  Bus reported description: {}", description.value_or("<error>"));

    // The DolphinBar slot this is could perhaps be worked out too (to keep the player LEDs it
    // set), but nothing needs it.
    std::optional<bool> is_balance_board;
    bool recognised = false;

    if (bluetooth_name.has_value())
    {
      if (IsWiimoteName(*bluetooth_name))
      {
        recognised = true;
        is_balance_board = false;
      }
      else if (IsBalanceBoardName(*bluetooth_name))
      {
        recognised = true;
        is_balance_board = true;
      }
    }
    else if (description.has_value())
    {
      recognised = *description == DOLPHINBAR_DESCRIPTION || IsValidDeviceName(*description);
    }

    // Third-party remotes copy the name (the Wii checks only that), so the ids are a fallback.
    if (!recognised)
    {
      const std::optional<USBUtils::DeviceInfo> ids = ReadDeviceIds(hid_iface);
      if (!ids.has_value())
        continue;

      DEBUG_LOG_FMT(WIIMOTE, "  HID ids: {}", ids->ToString());
      if (!IsKnownDeviceId(*ids))
        continue;
    }

    found.push_back({hid_iface, is_balance_board});
  }

  return found;
}
}  // namespace

void WiimoteScannerWindows::FindAndAuthenticateWiimotes()
{
  // Sync button pairing makes remotes reconnect at a button press later (1+2 pairing does not
  // really help), and a few rounds make a single click more likely to work.
  constexpr int ROUNDS = 3;

  RemoveUnusableWiimotes();

  u32 paired = 0;
  for (int round = 0; round != ROUNDS; ++round)
    paired += PairWiimotes(INQUIRY_LENGTH, PairingKey::HostAddress);

  NOTICE_LOG_FMT(WIIMOTE, "Paired Wii Remotes: {}", paired);
}

void WiimoteScannerWindows::RemoveRememberedWiimotes()
{
  const u32 removed =
      RemoveWiimotes([](const BLUETOOTH_DEVICE_INFO& device) { return device.fRemembered; });
  NOTICE_LOG_FMT(WIIMOTE, "Removed paired Wii Remotes: {}", removed);
}

WiimoteScannerWindows::WiimoteScannerWindows()
{
  m_device_change_notification.Register([this](Common::DeviceChangeNotification::EventType) {
    m_devices_changed.store(true, std::memory_order_release);
  });
}

void WiimoteScannerWindows::Update()
{
}

auto WiimoteScannerWindows::FindWiimoteHIDDevices() -> FindResults
{
  if (m_devices_changed.exchange(false, std::memory_order_acquire))
  {
    m_wiimote_hid_interfaces = ListWiimoteInterfaces();
    INFO_LOG_FMT(WIIMOTE, "{} Wii HID interface(s) present", m_wiimote_hid_interfaces.size());
  }

  FindResults found;

  for (EnumeratedWiimoteInterface& entry : m_wiimote_hid_interfaces)
  {
    if (!IsNewWiimote(WStringToUTF8(entry.hid_iface)))
      continue;

    DEBUG_LOG_FMT(WIIMOTE, "Trying a Wii HID interface");

    auto wiimote = std::make_unique<WiimoteWindows>(entry.hid_iface);
    if (!wiimote->ConnectInternal())
    {
      // Common: both Windows and the DolphinBar keep interfaces of absent remotes.
      DEBUG_LOG_FMT(WIIMOTE, "Nothing answered on that interface");
      continue;
    }

    // Checked once per interface, then remembered.
    if (!entry.is_balance_board.has_value())
      entry.is_balance_board = wiimote->IsBalanceBoard();

    auto& list = *entry.is_balance_board ? found.balance_boards : found.wii_remotes;
    list.push_back(std::move(wiimote));
  }

  return found;
}

auto WiimoteScannerWindows::FindNewWiimotes() -> FindResults
{
  RemoveUnusableWiimotes();
  PairWiimotes(INQUIRY_LENGTH);

  return FindWiimoteHIDDevices();
}

auto WiimoteScannerWindows::FindAttachedWiimotes() -> FindResults
{
  return FindWiimoteHIDDevices();
}

WiimoteWindows::WiimoteWindows(std::wstring hid_iface) : m_hid_iface{std::move(hid_iface)}
{
  // All auto-reset and initially unsignalled.
  m_hid_overlap_read.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  m_hid_overlap_write.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  m_wakeup_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

WiimoteWindows::~WiimoteWindows()
{
  Shutdown();

  CloseHandle(m_hid_overlap_read.hEvent);
  CloseHandle(m_hid_overlap_write.hEvent);
  CloseHandle(m_wakeup_event);
}

bool WiimoteWindows::ConnectInternal()
{
  if (IsConnected())
    return true;

  m_dev_handle = CreateFile(m_hid_iface.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED, nullptr);
  if (m_dev_handle == INVALID_HANDLE_VALUE)
    return false;

  // Opening proves little: Windows keeps interfaces of disconnected remotes around for minutes
  // (reads time out, the first write fails with ERROR_OPERATION_ABORTED), and the DolphinBar
  // always opens (writes fail with ERROR_GEN_FAILURE). So try a status request; its answer is
  // not needed.
  const u8 probe[] = {u8(OutputReportID::RequestStatus), 0};
  if (OverlappedWrite(probe, sizeof(probe)) <= 0)
  {
    CloseHandle(std::exchange(m_dev_handle, nullptr));
    return false;
  }

  m_is_connected.store(true, std::memory_order_relaxed);
  return true;
}

void WiimoteWindows::DisconnectInternal()
{
  if (!IsConnected())
    return;

  m_is_connected.store(false, std::memory_order_relaxed);
  CloseHandle(std::exchange(m_dev_handle, nullptr));
}

std::string WiimoteWindows::GetId() const
{
  return WStringToUTF8(m_hid_iface);
}

bool WiimoteWindows::IsConnected() const
{
  return m_is_connected.load(std::memory_order_relaxed);
}

int WiimoteWindows::OverlappedRead(u8* data, DWORD size)
{
  DWORD transferred = 0;
  if (ReadFile(m_dev_handle, data, size, &transferred, &m_hid_overlap_read))
    return static_cast<int>(transferred);

  const DWORD error = GetLastError();
  if (error != ERROR_IO_PENDING)
  {
    ERROR_LOG_FMT(WIIMOTE, "Reading from Wii Remote {} failed: {}", m_index + 1,
                  Common::GetWin32ErrorString(error));
    return 0;
  }

  // Wait for the read, a wakeup or the timeout; anything but a finished read cancels it.
  const std::array<HANDLE, 2> events{m_wakeup_event, m_hid_overlap_read.hEvent};
  const DWORD signalled = WaitForMultipleObjects(DWORD(events.size()), events.data(), FALSE,
                                                 WIIMOTE_DEFAULT_TIMEOUT);
  const bool finished = signalled == WAIT_OBJECT_0 + 1;
  if (signalled == WAIT_OBJECT_0)
  {
    DEBUG_LOG_FMT(WIIMOTE, "Read interrupted by IOWakeup");
  }
  else if (signalled == WAIT_TIMEOUT)
  {
    DEBUG_LOG_FMT(WIIMOTE, "Read timed out");
  }
  else if (!finished)
  {
    ERROR_LOG_FMT(WIIMOTE, "Waiting for a read failed: {}", Common::GetLastErrorString());
  }

  if (!finished)
    CancelIo(m_dev_handle);

  // The read either finished or is being cancelled; for a cancel, wait until it is done.
  if (GetOverlappedResult(m_dev_handle, &m_hid_overlap_read, &transferred, !finished))
    return static_cast<int>(transferred);

  const DWORD result_error = GetLastError();
  // Cancelled by the timeout or IOWakeup: nothing read, but nothing wrong either.
  if (result_error == ERROR_OPERATION_ABORTED)
    return -1;

  DEBUG_LOG_FMT(WIIMOTE, "Read failed: {}", Common::GetWin32ErrorString(result_error));
  return 0;
}

int WiimoteWindows::IORead(u8* buf)
{
  // Windows leaves out the HID transaction header (DATA | INPUT), so put it in.
  constexpr u8 HID_HEADER_DATA_INPUT = 0xa1;
  buf[0] = HID_HEADER_DATA_INPUT;

  const int read = OverlappedRead(buf + 1, MAX_PAYLOAD - 1);
  DEBUG_LOG_FMT(WIIMOTE, "Read {} bytes", read);
  if (read <= 0)
    return read;

  const u8 report_id = buf[1];
  const size_t payload_size = InputReportSize(report_id);
  if (payload_size == 0)
  {
    ERROR_LOG_FMT(WIIMOTE, "Wii Remote {} sent unknown report 0x{:02x}", m_index + 1, report_id);
    return -1;
  }

  // Header byte, report id, payload.
  return static_cast<int>(2 + payload_size);
}

void WiimoteWindows::IOWakeup()
{
  SetEvent(m_wakeup_event);
}

int WiimoteWindows::OverlappedWrite(const u8* data, DWORD size)
{
  DWORD transferred = 0;
  if (WriteFile(m_dev_handle, data, size, &transferred, &m_hid_overlap_write))
    return static_cast<int>(transferred);

  const DWORD error = GetLastError();
  if (error != ERROR_IO_PENDING)
  {
    ERROR_LOG_FMT(WIIMOTE, "Writing to Wii Remote {} failed: {}", m_index + 1,
                  Common::GetWin32ErrorString(error));
    return 0;
  }

  const bool finished =
      WaitForSingleObject(m_hid_overlap_write.hEvent, WIIMOTE_DEFAULT_TIMEOUT) == WAIT_OBJECT_0;
  if (!finished)
  {
    ERROR_LOG_FMT(WIIMOTE, "Waiting for a write failed: {}", Common::GetLastErrorString());
    CancelIo(m_dev_handle);
  }

  if (GetOverlappedResult(m_dev_handle, &m_hid_overlap_write, &transferred, !finished))
    return static_cast<int>(transferred);

  // A DolphinBar slot without a remote fails here with ERROR_GEN_FAILURE.
  DEBUG_LOG_FMT(WIIMOTE, "Write failed: {}", Common::GetLastErrorString());
  return 0;
}

int WiimoteWindows::IOWrite(const u8* buf, size_t len)
{
  DEBUG_ASSERT(len > 0);

  // Windows adds the HID transaction header itself.
  const int written = OverlappedWrite(buf + 1, DWORD(len - 1));
  DEBUG_LOG_FMT(WIIMOTE, "Wrote {} bytes", written);
  return written;
}
}  // namespace WiimoteReal
