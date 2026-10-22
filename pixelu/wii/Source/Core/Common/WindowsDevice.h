// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef _WIN32

#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <setupapi.h>

#include "Common/Functional.h"

namespace Common
{
// A string-typed property of a device node / device interface, or nullopt if it is missing or
// not a string.
std::optional<std::wstring> GetDevNodeStringProperty(DEVINST device,
                                                     const DEVPROPKEY* requested_property);
std::optional<std::wstring> GetDeviceInterfaceStringProperty(LPCWSTR iface,
                                                             const DEVPROPKEY* requested_property);

// Owns a Windows "multi-string" (strings each ending in a NUL, the list ending in an extra NUL)
// and iterates its strings: `for (WCHAR* str : list)`.
template <typename T>
struct NullTerminatedStringList
{
  class Iterator
  {
    friend NullTerminatedStringList;

  public:
    constexpr T* operator*() { return m_ptr; }

    constexpr Iterator& operator++()
    {
      // Step past this string and its terminator.
      m_ptr += std::basic_string_view<T>(m_ptr).size() + 1;
      return *this;
    }

    constexpr Iterator operator++(int)
    {
      Iterator previous = *this;
      ++*this;
      return previous;
    }

    // The list ends at an empty string.
    constexpr bool operator==(std::default_sentinel_t) const { return *m_ptr == T{}; }

  private:
    constexpr Iterator(T* ptr) : m_ptr{ptr} {}

    T* m_ptr;
  };

  constexpr auto begin() const { return Iterator{list.get()}; }
  constexpr auto end() const { return std::default_sentinel; }

  std::unique_ptr<T[]> list;
};

// The device interfaces of a class (CM_Get_Device_Interface_List). Empty on failure.
NullTerminatedStringList<WCHAR> GetDeviceInterfaceList(LPGUID iface_class_guid, DEVINSTID device_id,
                                                       ULONG flags);

// Calls back when HID device interfaces arrive or are removed.
class DeviceChangeNotification
{
public:
  enum class EventType : bool
  {
    Arrival,
    Removal,
  };
  using CallbackType = Common::MoveOnlyFunction<void(EventType)>;

  DeviceChangeNotification();
  ~DeviceChangeNotification();

  // Replaces any previous registration. The callback runs on a system thread.
  void Register(CallbackType callback);
  void Unregister();

  DeviceChangeNotification(const DeviceChangeNotification&) = delete;
  DeviceChangeNotification(DeviceChangeNotification&&) = delete;
  void operator=(const DeviceChangeNotification&) = delete;
  void operator=(DeviceChangeNotification&&) = delete;

private:
  CallbackType m_callback{};
  HCMNOTIFICATION m_notify_handle{nullptr};
};

}  // namespace Common

#endif
