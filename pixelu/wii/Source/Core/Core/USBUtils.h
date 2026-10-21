// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

struct libusb_device_descriptor;

namespace USBUtils
{
// A USB vendor/product ID pair.
struct DeviceInfo
{
  u16 vid;
  u16 pid;

  constexpr auto operator<=>(const DeviceInfo& other) const = default;

  // Parses "VID:PID" (hexadecimal).
  static std::optional<DeviceInfo> FromString(const std::string& str);
  // "vvvv:pppp"
  std::string ToString() const;
  // "Name (vvvv:pppp)", with the name looked up.
  std::string ToDisplayString() const;
  std::string ToDisplayString(const std::string& name) const;
};

// Finds a human-readable name for a device, from a built-in list of Wii peripherals or the
// operating system.
std::optional<std::string> GetDeviceNameFromVIDPID(u16 vid, u16 pid);

// The connected USB devices accepted by `filter`.
std::vector<DeviceInfo>
ListDevices(const std::function<bool(const struct libusb_device_descriptor&)>& filter =
                [](const struct libusb_device_descriptor&) { return true; });
std::vector<DeviceInfo> ListDevices(const std::function<bool(const DeviceInfo&)>& filter =
                                        [](const DeviceInfo&) { return true; });

}  // namespace USBUtils
