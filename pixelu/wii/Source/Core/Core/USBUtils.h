// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

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

}  // namespace USBUtils
