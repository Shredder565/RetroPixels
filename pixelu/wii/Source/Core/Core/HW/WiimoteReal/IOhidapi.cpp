// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteReal/IOhidapi.h"

#include <cerrno>
#include <string>

#if defined(__linux__)
#include <filesystem>
#include <system_error>
#endif

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

// hidapi passes reports without the HID transaction header byte, which the rest of the code uses,
// so reads put it back and writes leave it out.

namespace WiimoteReal
{
using WiimoteCommon::OutputReportID;

namespace
{
// hidapi strings may be missing.
std::string ToUTF8(const wchar_t* str)
{
  return str != nullptr ? WStringToUTF8(str) : std::string{};
}

void LogOpenFailure(const std::string& device_path)
{
  ERROR_LOG_FMT(WIIMOTE,
                "Could not open the Wii Remote at \"{}\". "
                "Do you have permission to access the device?",
                device_path);
}

#if defined(__linux__)
// Only for the log: the kernel's own "wiimote" driver competes with us for the device, and
// rebinding it to hid-generic needs root.
void LogLinuxDriver(const char* device_path)
{
  namespace fs = std::filesystem;

  // /sys/class/hidraw/<node>/device/driver links to /sys/bus/hid/drivers/<driver>.
  const fs::path driver_link =
      fs::path("/sys/class/hidraw") / fs::path(device_path).filename() / "device" / "driver";

  std::error_code error;
  const fs::path driver = fs::canonical(driver_link, error);
  if (error)
  {
    WARN_LOG_FMT(WIIMOTE, "Could not find out which driver {} uses: {}", device_path,
                 error.message());
    return;
  }

  const fs::path driver_name = driver.filename();
  if (driver_name == "wiimote")
  {
    ERROR_LOG_FMT(WIIMOTE, "The Wii Remote at {} is bound to the Linux 'wiimote' driver.",
                  device_path);
    return;
  }

  DEBUG_LOG_FMT(WIIMOTE, "The Wii Remote at {} uses the {} driver", device_path,
                driver_name.string());
}
#endif

// Adapters such as the DolphinBar always show four HID devices, connected remote or not, so
// a status request is sent to see if anything answers.
bool CanTalkTo(const std::string& device_path)
{
  hid_device* const device = hid_open_path(device_path.c_str());
  if (device == nullptr)
  {
    LogOpenFailure(device_path);
    return false;
  }

  static const u8 status_request[] = {u8(OutputReportID::RequestStatus), 0};
  const int written = hid_write(device, status_request, sizeof(status_request));
  // EPIPE is how the DolphinBar says that no remote is there, which is not worth an error.
  if (written == -1 && errno != EPIPE)
    ERROR_LOG_FMT(WIIMOTE, "Could not write to the Wii Remote at \"{}\".", device_path);

  hid_close(device);
  return written != -1;
}
}  // namespace

WiimoteScannerHidapi::WiimoteScannerHidapi()
{
#ifdef __OpenBSD__
  // OpenBSD's hidapi package renames hid_init, which clashes with the system's usbhid library.
  const int init_result = hidapi_hid_init();
#else
  const int init_result = hid_init();
#endif
  ASSERT_MSG(WIIMOTE, init_result == 0, "hidapi could not be initialized.");
}

WiimoteScannerHidapi::~WiimoteScannerHidapi()
{
  if (hid_exit() == -1)
    ERROR_LOG_FMT(WIIMOTE, "hidapi could not be shut down.");
}

auto WiimoteScannerHidapi::FindAttachedWiimotes() -> FindResults
{
  FindResults found;

  // Vendor and product 0 list every HID device.
  hid_device_info* const devices = hid_enumerate(0, 0);
  for (const hid_device_info* info = devices; info != nullptr; info = info->next)
  {
    const std::string product = ToUTF8(info->product_string);
    const bool looks_like_wiimote =
        IsValidDeviceName(product) || IsKnownDeviceId({info->vendor_id, info->product_id});
    if (!looks_like_wiimote || !IsNewWiimote(info->path) || !CanTalkTo(info->path))
      continue;

#if defined(__linux__)
    LogLinuxDriver(info->path);
#endif

    auto device = std::make_unique<WiimoteHidapi>(info->path);
    const bool balance_board = IsBalanceBoardName(product) || device->IsBalanceBoard();

    NOTICE_LOG_FMT(WIIMOTE, "Found a {} at {}: {} {} ({:04x}:{:04x})",
                   balance_board ? "balance board" : "Wii Remote", info->path,
                   ToUTF8(info->manufacturer_string), product, info->vendor_id, info->product_id);

    if (balance_board)
      found.balance_boards.push_back(std::move(device));
    else
      found.wii_remotes.push_back(std::move(device));
  }
  hid_free_enumeration(devices);

  return found;
}

WiimoteHidapi::WiimoteHidapi(std::string device_path) : m_device_path(std::move(device_path))
{
}

WiimoteHidapi::~WiimoteHidapi()
{
  Shutdown();
}

bool WiimoteHidapi::ConnectInternal()
{
  if (m_handle == nullptr)
  {
    m_handle = hid_open_path(m_device_path.c_str());
    if (m_handle == nullptr)
      LogOpenFailure(m_device_path);
  }
  return m_handle != nullptr;
}

void WiimoteHidapi::DisconnectInternal()
{
  hid_close(m_handle);
  m_handle = nullptr;
}

bool WiimoteHidapi::IsConnected() const
{
  return m_handle != nullptr;
}

int WiimoteHidapi::IORead(u8* buf)
{
  constexpr int READ_TIMEOUT_MS = 200;

  const int length = hid_read_timeout(m_handle, buf + 1, MAX_PAYLOAD - 1, READ_TIMEOUT_MS);
  switch (length)
  {
  case -1:
    ERROR_LOG_FMT(WIIMOTE, "Reading from {} failed.", m_device_path);
    return 0;
  case 0:
    // Timed out.
    return -1;
  default:
    buf[0] = WR_SET_REPORT | BT_INPUT;
    return length + 1;
  }
}

int WiimoteHidapi::IOWrite(const u8* buf, size_t len)
{
  DEBUG_ASSERT(len > 0);
  DEBUG_ASSERT(buf[0] == (WR_SET_REPORT | BT_OUTPUT));

  const int written = hid_write(m_handle, buf + 1, len - 1);
  if (written == -1)
  {
    ERROR_LOG_FMT(WIIMOTE, "Writing to {} failed.", m_device_path);
    return 0;
  }
  // A write of zero bytes still counts as success.
  return written == 0 ? 1 : written;
}
}  // namespace WiimoteReal
