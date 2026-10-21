// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/USBUtils.h"

#include <array>
#include <charconv>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#ifdef HAVE_LIBUDEV
#include <libudev.h>
#endif
#ifdef __LIBUSB__
#include <libusb.h>
#endif
#ifdef _WIN32
#include <cfgmgr32.h>
#include <devpkey.h>
#include <initguid.h>
// usbiodef.h defines its GUIDs through initguid.h, which must come first.
#include <usbiodef.h>

#include <fmt/xchar.h>

#include "Common/StringUtil.h"
#include "Common/WindowsDevice.h"
#endif

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/ScopeGuard.h"
#include "Core/LibusbUtils.h"

namespace USBUtils
{
namespace
{
// Names for the USB devices Wii games are known to use.
const std::map<DeviceInfo, std::string> s_known_devices{{
    {{0x046d, 0x0a03}, "Logitech Microphone"},
    {{0x057e, 0x0308}, "Wii Speak"},
    {{0x057e, 0x0309}, "Nintendo USB Microphone"},
    {{0x057e, 0x030a}, "Ubisoft Motion Tracking Camera"},
    {{0x0e6f, 0x0129}, "Disney Infinity Reader (Portal Device)"},
    {{0x12ba, 0x0200}, "Harmonix Guitar for PlayStation 3"},
    {{0x12ba, 0x0210}, "Harmonix Drum Kit for PlayStation 3"},
    {{0x12ba, 0x0218}, "Harmonix Drum Kit for PlayStation 3"},
    {{0x12ba, 0x2330}, "Harmonix RB3 Keyboard for PlayStation 3"},
    {{0x12ba, 0x2338}, "Harmonix RB3 MIDI Keyboard Interface for PlayStation 3"},
    {{0x12ba, 0x2430}, "Harmonix RB3 Mustang Guitar for PlayStation 3"},
    {{0x12ba, 0x2438}, "Harmonix RB3 MIDI Guitar Interface for PlayStation 3"},
    {{0x12ba, 0x2530}, "Harmonix RB3 Squier Guitar for PlayStation 3"},
    {{0x12ba, 0x2538}, "Harmonix RB3 MIDI Guitar Interface for PlayStation 3"},
    {{0x1430, 0x0100}, "Tony Hawk Ride Skateboard"},
    {{0x1430, 0x0150}, "Skylanders Portal"},
    {{0x1bad, 0x0004}, "Harmonix Guitar Controller for Nintendo Wii"},
    {{0x1bad, 0x0005}, "Harmonix Drum Controller for Nintendo Wii"},
    {{0x1bad, 0x3010}, "Harmonix Guitar Controller for Nintendo Wii"},
    {{0x1bad, 0x3110}, "Harmonix Drum Controller for Nintendo Wii"},
    {{0x1bad, 0x3138}, "Harmonix Drum Controller for Nintendo Wii"},
    {{0x1bad, 0x3330}, "Harmonix RB3 Keyboard for Nintendo Wii"},
    {{0x1bad, 0x3338}, "Harmonix RB3 MIDI Keyboard Interface for Nintendo Wii"},
    {{0x1bad, 0x3430}, "Harmonix RB3 Mustang Guitar for Nintendo Wii"},
    {{0x1bad, 0x3438}, "Harmonix RB3 MIDI Guitar Interface for Nintendo Wii"},
    {{0x1bad, 0x3530}, "Harmonix RB3 Squier Guitar for Nintendo Wii"},
    {{0x1bad, 0x3538}, "Harmonix RB3 MIDI Guitar Interface for Nintendo Wii"},
    {{0x21a4, 0xac40}, "EA Active NFL"},
}};

// Parses a leading hexadecimal number (trailing characters are ignored).
bool ParseHexPrefix(std::string_view text, u16* value)
{
  return std::from_chars(text.data(), text.data() + text.size(), *value, 16).ec == std::errc{};
}

std::optional<std::string> NameFromBuiltInList(u16 vid, u16 pid)
{
  if (const auto found = s_known_devices.find(DeviceInfo{vid, pid}); found != s_known_devices.end())
    return found->second;
  return std::nullopt;
}

#ifdef HAVE_LIBUDEV
// The udev hardware database (usb.ids).
std::optional<std::string> NameFromHardwareDatabase(u16 vid, u16 pid)
{
  udev* const context = udev_new();
  if (context == nullptr)
    return std::nullopt;
  Common::ScopeGuard release_context{[&] { udev_unref(context); }};

  udev_hwdb* const database = udev_hwdb_new(context);
  if (database == nullptr)
    return std::nullopt;
  Common::ScopeGuard release_database{[&] { udev_hwdb_unref(database); }};

  const std::string modalias = fmt::format("usb:v{:04X}p{:04X}*", vid, pid);
  udev_list_entry* const properties =
      udev_hwdb_get_properties_list_entry(database, modalias.c_str(), 0);
  if (properties == nullptr)
    return std::nullopt;

  udev_list_entry* const model = udev_list_entry_get_by_name(properties, "ID_MODEL_FROM_DATABASE");
  if (model == nullptr)
    return std::nullopt;
  return std::string(udev_list_entry_get_value(model));
}
#endif

#ifdef _WIN32
// The device's description as Windows' device manager knows it.
std::optional<std::string> NameFromConfigManager(u16 vid, u16 pid)
{
  GUID usb_devices = GUID_DEVINTERFACE_USB_DEVICE;
  const std::wstring id_part = fmt::format(L"VID_{:04X}&PID_{:04X}", vid, pid);

  for (const wchar_t* interface_path :
       Common::GetDeviceInterfaceList(&usb_devices, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT))
  {
    if (!std::wstring_view{interface_path}.contains(id_part))
      continue;

    // Only the first matching interface is looked at.
    std::optional<std::wstring> instance_id =
        Common::GetDeviceInterfaceStringProperty(interface_path, &DEVPKEY_Device_InstanceId);
    if (!instance_id)
      return std::nullopt;

    DEVINST node{};
    if (CM_Locate_DevNode(&node, instance_id->data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
    {
      ERROR_LOG_FMT(COMMON, "CM_Locate_DevNode");
      return std::nullopt;
    }

    // The bus-reported description is usually the product name; DeviceDesc is often generic
    // ("USB Input Device"), so it is only the fallback.
    std::optional<std::wstring> description =
        Common::GetDevNodeStringProperty(node, &DEVPKEY_Device_BusReportedDeviceDesc);
    if (!description)
      description = Common::GetDevNodeStringProperty(node, &DEVPKEY_Device_DeviceDesc);
    if (!description)
      return std::nullopt;
    return WStringToUTF8(*description);
  }
  return std::nullopt;
}
#endif

// Opening devices through libusb can crash Windows with some Bluetooth miniport drivers
// (those built on BthUsb.sys, such as Qualcomm Atheros' btfilter.sys and Intel's ibtusb.sys), so
// this lookup is not used there.
#if defined(__LIBUSB__) && !defined(_WIN32)
// Reads the product string from the device itself.
std::optional<std::string> NameFromDevice(u16 vid, u16 pid)
{
  LibusbUtils::Context context;
  if (!context.IsValid())
    return std::nullopt;

  std::optional<std::string> name;
  context.GetDeviceList([&](libusb_device* device) {
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS)
      return true;
    if (descriptor.idVendor != vid || descriptor.idProduct != pid)
      return true;

    // The first matching device decides.
    if (descriptor.iProduct == 0)
      return false;
    libusb_device_handle* handle = nullptr;
    if (libusb_open(device, &handle) != LIBUSB_SUCCESS)
      return false;
    name = LibusbUtils::GetStringDescriptor(handle, descriptor.iProduct);
    libusb_close(handle);
    return false;
  });
  return name;
}
#endif
}  // namespace

std::optional<DeviceInfo> DeviceInfo::FromString(const std::string& str)
{
  const std::size_t colon = str.find(':');
  if (colon == std::string::npos)
    return std::nullopt;

  const std::string_view text{str};
  DeviceInfo info;
  if (!ParseHexPrefix(text.substr(0, colon), &info.vid) ||
      !ParseHexPrefix(text.substr(colon + 1), &info.pid))
  {
    return std::nullopt;
  }
  return info;
}

std::string DeviceInfo::ToString() const
{
  return fmt::format("{:04x}:{:04x}", vid, pid);
}

std::string DeviceInfo::ToDisplayString() const
{
  std::optional<std::string> name = GetDeviceNameFromVIDPID(vid, pid);
  if (!name)
  {
    // i18n: This replaces the name of a device if it cannot be found.
    name = Common::GetStringT("Unknown Device");
  }
  return ToDisplayString(*name);
}

std::string DeviceInfo::ToDisplayString(const std::string& name) const
{
  return fmt::format("{} ({:04x}:{:04x})", name, vid, pid);
}

std::optional<std::string> GetDeviceNameFromVIDPID(u16 vid, u16 pid)
{
  // Cheapest and most reliable first; asking the device itself is the slowest.
  using Lookup = std::optional<std::string> (*)(u16, u16);
  static constexpr Lookup lookups[] = {
      &NameFromBuiltInList,
#ifdef HAVE_LIBUDEV
      &NameFromHardwareDatabase,
#endif
#ifdef _WIN32
      &NameFromConfigManager,
#endif
#if defined(__LIBUSB__) && !defined(_WIN32)
      &NameFromDevice,
#endif
  };

  for (const Lookup lookup : lookups)
  {
    if (std::optional<std::string> name = lookup(vid, pid))
      return name;
  }
  return std::nullopt;
}

#ifdef __LIBUSB__
std::vector<DeviceInfo>
ListDevices(const std::function<bool(const libusb_device_descriptor&)>& filter)
{
  LibusbUtils::Context context;
  if (!context.IsValid())
    return {};

  std::vector<DeviceInfo> devices;
  const int result = context.GetDeviceList([&](libusb_device* device) {
    libusb_device_descriptor descriptor;
    if (libusb_get_device_descriptor(device, &descriptor) == LIBUSB_SUCCESS && filter(descriptor))
      devices.push_back(DeviceInfo{descriptor.idVendor, descriptor.idProduct});
    return true;
  });
  if (result != LIBUSB_SUCCESS)
    WARN_LOG_FMT(CORE, "Failed to get device list: {}", LibusbUtils::ErrorWrap(result));

  return devices;
}

std::vector<DeviceInfo> ListDevices(const std::function<bool(const DeviceInfo&)>& filter)
{
  return ListDevices([&](const libusb_device_descriptor& descriptor) {
    return filter(DeviceInfo{descriptor.idVendor, descriptor.idProduct});
  });
}
#endif
}  // namespace USBUtils
