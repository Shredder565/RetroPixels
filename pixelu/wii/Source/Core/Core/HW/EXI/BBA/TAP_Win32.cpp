// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/BBA/TAP_Win32.h"

#include <optional>
#include <string>
#include <vector>

#include <tchar.h>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"

namespace ExpansionInterface
{
namespace
{
// Owns an open registry key.
class RegistryKey
{
public:
  RegistryKey(HKEY parent, const std::wstring& path, REGSAM access)
  {
    if (RegOpenKeyExW(parent, path.c_str(), 0, access, &m_key) != ERROR_SUCCESS)
      m_key = nullptr;
  }
  ~RegistryKey()
  {
    if (m_key != nullptr)
      RegCloseKey(m_key);
  }
  RegistryKey(const RegistryKey&) = delete;
  RegistryKey& operator=(const RegistryKey&) = delete;

  bool IsOpen() const { return m_key != nullptr; }

  std::optional<std::wstring> SubkeyName(DWORD index) const
  {
    wchar_t name[256];
    DWORD length = static_cast<DWORD>(std::size(name));
    if (RegEnumKeyExW(m_key, index, name, &length, nullptr, nullptr, nullptr, nullptr) !=
        ERROR_SUCCESS)
    {
      return std::nullopt;
    }
    return std::wstring(name, length);
  }

  std::optional<std::wstring> String(const wchar_t* value_name) const
  {
    wchar_t data[256];
    DWORD size = sizeof(data);
    DWORD type = 0;
    if (RegQueryValueExW(m_key, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(data),
                         &size) != ERROR_SUCCESS ||
        type != REG_SZ)
    {
      return std::nullopt;
    }
    // The stored string may or may not include its terminator.
    std::wstring result(data, size / sizeof(wchar_t));
    while (!result.empty() && result.back() == L'\0')
      result.pop_back();
    return result;
  }

  DWORD SubkeyCount() const
  {
    DWORD count = 0;
    if (RegQueryInfoKeyW(m_key, nullptr, nullptr, nullptr, &count, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
    {
      return 0;
    }
    return count;
  }

private:
  HKEY m_key = nullptr;
};

// True if the network adapter class lists an adapter with this instance GUID whose component ID
// names the TAP-Windows driver.
bool IsTapAdapter(const std::wstring& guid)
{
  const RegistryKey adapters(HKEY_LOCAL_MACHINE, ADAPTER_KEY, KEY_READ);
  if (!adapters.IsOpen())
    return false;

  const std::wstring driver_id = TAP_COMPONENT_ID;
  for (DWORD index = 0;; ++index)
  {
    const std::optional<std::wstring> subkey = adapters.SubkeyName(index);
    if (!subkey)
      return false;

    const RegistryKey adapter(HKEY_LOCAL_MACHINE, std::wstring(ADAPTER_KEY) + L"\\" + *subkey,
                              KEY_READ);
    if (!adapter.IsOpen())
      continue;

    const std::optional<std::wstring> component = adapter.String(L"ComponentId");
    const std::optional<std::wstring> instance = adapter.String(L"NetCfgInstanceId");
    if (!component || !instance)
      continue;

    // The component ID must end in the driver's ID (e.g. "root\tap0901").
    const std::size_t found = component->find(driver_id);
    if (found != std::wstring::npos && component->compare(found, std::wstring::npos, driver_id) == 0 &&
        *instance == guid)
    {
      return true;
    }
  }
}

// The GUIDs of all network connections that are TAP adapters.
std::vector<std::wstring> FindTapAdapters()
{
  std::vector<std::wstring> guids;
  const RegistryKey connections(HKEY_LOCAL_MACHINE, NETWORK_CONNECTIONS_KEY,
                                KEY_READ | KEY_QUERY_VALUE);
  if (!connections.IsOpen())
    return guids;

  const DWORD count = connections.SubkeyCount();
  for (DWORD index = 0; index < count; ++index)
  {
    const std::optional<std::wstring> guid = connections.SubkeyName(index);
    if (!guid)
      continue;

    // Only connections with a name are real adapters.
    const RegistryKey connection(HKEY_LOCAL_MACHINE,
                                 std::wstring(NETWORK_CONNECTIONS_KEY) + L"\\" + *guid +
                                     L"\\Connection",
                                 KEY_READ);
    if (!connection.IsOpen() || !connection.String(L"Name"))
      continue;

    if (IsTapAdapter(*guid))
      guids.push_back(*guid);
  }
  return guids;
}

HANDLE OpenTapDevice(const std::wstring& guid)
{
  const std::wstring path = std::wstring(USERMODEDEVICEDIR) + guid + TAPSUFFIX;
  const HANDLE device = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED,
                                    nullptr);
  if (device == INVALID_HANDLE_VALUE)
    INFO_LOG_FMT(SP1, "Failed to open TAP at {}", WStringToUTF8(path));
  return device;
}
}  // namespace

bool CEXIETHERNET::TAPNetworkInterface::Activate()
{
  if (IsActivated())
    return true;

  const std::vector<std::wstring> adapters = FindTapAdapters();
  if (adapters.empty())
  {
    ERROR_LOG_FMT(SP1, "Failed to find a TAP GUID");
    return false;
  }

  for (const std::wstring& guid : adapters)
  {
    m_adapter = OpenTapDevice(guid);
    if (m_adapter != INVALID_HANDLE_VALUE)
    {
      INFO_LOG_FMT(SP1, "OPENED {}", WStringToUTF8(guid));
      break;
    }
  }
  if (m_adapter == INVALID_HANDLE_VALUE)
  {
    PanicAlertFmt("Failed to open any TAP");
    return false;
  }

  DWORD returned = 0;
  ULONG version[3]{};
  if (DeviceIoControl(m_adapter, TAP_IOCTL_GET_VERSION, &version, sizeof(version), &version,
                      sizeof(version), &returned, nullptr))
  {
    INFO_LOG_FMT(SP1, "TAP-Win32 Driver Version {}.{} {}", version[0], version[1],
                 version[2] ? "(DEBUG)" : "");
  }
  const bool new_enough = version[0] > TAP_WIN32_MIN_MAJOR ||
                          (version[0] == TAP_WIN32_MIN_MAJOR && version[1] >= TAP_WIN32_MIN_MINOR);
  if (!new_enough)
  {
    PanicAlertFmtT("ERROR: This version of Dolphin requires a TAP-Win32 driver"
                   " that is at least version {0}.{1} -- If you recently upgraded your Dolphin"
                   " distribution, a reboot is probably required at this point to get"
                   " Windows to see the new driver.",
                   TAP_WIN32_MIN_MAJOR, TAP_WIN32_MIN_MINOR);
    return false;
  }

  // Tell the driver the "cable" is plugged in.
  ULONG media_connected = TRUE;
  if (!DeviceIoControl(m_adapter, TAP_IOCTL_SET_MEDIA_STATUS, &media_connected,
                       sizeof(media_connected), &media_connected, sizeof(media_connected),
                       &returned, nullptr))
  {
    ERROR_LOG_FMT(SP1, "WARNING: The TAP-Win32 driver rejected a"
                       "TAP_IOCTL_SET_MEDIA_STATUS DeviceIoControl call.");
    return false;
  }

  m_read_io.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  m_write_io.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (m_read_io.hEvent == nullptr || m_write_io.hEvent == nullptr)
    return false;

  m_write_buffer.reserve(1518);
  return RecvInit();
}

void CEXIETHERNET::TAPNetworkInterface::Deactivate()
{
  if (!IsActivated())
    return;

  m_receiving.Clear();
  m_stop_reading.Set();

  // Abort the reader's pending read (and any pending write) so the thread can see the flag.
  CancelIoEx(m_adapter, nullptr);
  if (m_read_thread.joinable())
    m_read_thread.join();

  CloseHandle(m_read_io.hEvent);
  CloseHandle(m_write_io.hEvent);
  CloseHandle(m_adapter);
  m_adapter = INVALID_HANDLE_VALUE;
  m_read_io = {};
  m_write_io = {};
}

bool CEXIETHERNET::TAPNetworkInterface::IsActivated()
{
  return m_adapter != INVALID_HANDLE_VALUE;
}

void CEXIETHERNET::TAPNetworkInterface::ReadLoop()
{
  while (!m_stop_reading.IsSet())
  {
    DWORD length = 0;
    if (ReadFile(m_adapter, m_read_buffer.data(), static_cast<DWORD>(m_read_buffer.size()),
                 &length, &m_read_io))
    {
      // Finished at once; the event was not reset by the system then.
      ResetEvent(m_read_io.hEvent);
    }
    else if (GetLastError() != ERROR_IO_PENDING)
    {
      ERROR_LOG_FMT(SP1, "ReadFile failed (err={:#x})", GetLastError());
      continue;
    }
    else if (!GetOverlappedResult(m_adapter, &m_read_io, &length, TRUE))
    {
      // Aborted reads are how Deactivate stops the loop.
      if (GetLastError() != ERROR_OPERATION_ABORTED)
        ERROR_LOG_FMT(SP1, "GetOverlappedResult failed (err={:#x})", GetLastError());
      continue;
    }

    DEBUG_LOG_FMT(SP1, "Received {} bytes:\n {}", length,
                  ArrayToString(m_read_buffer.data(), length, 0x10));
    if (m_receiving.IsSet())
      m_eth_ref->ReceiveFrame(m_read_buffer.data(), length);
  }
}

bool CEXIETHERNET::TAPNetworkInterface::SendFrame(const u8* frame, u32 size)
{
  DEBUG_LOG_FMT(SP1, "SendFrame {} bytes:\n{}", size, ArrayToString(frame, size, 0x10));

  // Only one write can be outstanding; wait for the previous one.
  DWORD written = 0;
  if (m_write_in_flight && !GetOverlappedResult(m_adapter, &m_write_io, &written, TRUE))
    ERROR_LOG_FMT(SP1, "GetOverlappedResult failed (err={:#x})", GetLastError());

  m_write_buffer.assign(frame, frame + size);
  m_write_in_flight = true;

  if (WriteFile(m_adapter, m_write_buffer.data(), size, &written, &m_write_io))
  {
    ResetEvent(m_write_io.hEvent);
  }
  else if (GetLastError() != ERROR_IO_PENDING)
  {
    ERROR_LOG_FMT(SP1, "WriteFile failed (err={:#x})", GetLastError());
    ResetEvent(m_write_io.hEvent);
    m_write_in_flight = false;
    return false;
  }

  // The write is still running, but the adapter treats the frame as sent.
  m_eth_ref->SendComplete();
  return true;
}

bool CEXIETHERNET::TAPNetworkInterface::RecvInit()
{
  m_read_thread = std::thread(&TAPNetworkInterface::ReadLoop, this);
  return true;
}

void CEXIETHERNET::TAPNetworkInterface::RecvStart()
{
  m_receiving.Set();
}

void CEXIETHERNET::TAPNetworkInterface::RecvStop()
{
  m_receiving.Clear();
}
}  // namespace ExpansionInterface
