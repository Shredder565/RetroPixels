// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <fmt/format.h>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "Common/CommonTypes.h"

struct libusb_config_descriptor;
struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

// Small RAII helpers around libusb. Without libusb (no __LIBUSB__) everything reports failure.
namespace LibusbUtils
{
template <typename T>
using UniquePtr = std::unique_ptr<T, void (*)(T*)>;

// Called for each device; returning false stops the enumeration.
using GetDeviceListCallback = std::function<bool(libusb_device* device)>;

// A libusb context with its own event handling thread.
class Context
{
public:
  Context();
  ~Context();

  operator libusb_context*() const;
  bool IsValid() const;

  // Requires a valid context. Returns a libusb error code.
  int GetDeviceList(const GetDeviceListCallback& callback) const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

using ConfigDescriptor = UniquePtr<libusb_config_descriptor>;
// Returns the libusb result and, on success, the descriptor.
std::pair<int, ConfigDescriptor> MakeConfigDescriptor(libusb_device* device, u8 config_num = 0);

// Formats a libusb error code (libusb functions return plain ints) with fmt.
struct ErrorWrap
{
  constexpr explicit ErrorWrap(int error) : m_error(error) {}
  const int m_error;

  const char* GetStrError() const;
  const char* GetName() const;
};

// Reads a string descriptor (in the device's first language) as UTF-8.
std::optional<std::string> GetStringDescriptor(libusb_device_handle* dev_handle,
                                               uint8_t desc_index);

}  // namespace LibusbUtils

template <>
struct fmt::formatter<LibusbUtils::ErrorWrap>
{
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
  template <typename FormatContext>
  auto format(const LibusbUtils::ErrorWrap& error, FormatContext& ctx) const
  {
    // "<description> (<code>: <name>)"
    return fmt::format_to(ctx.out(), "{} ({}: {})", error.GetStrError(), error.m_error,
                          error.GetName());
  }
};
