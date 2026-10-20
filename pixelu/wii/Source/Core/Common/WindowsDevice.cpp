// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef _WIN32

#include "Common/WindowsDevice.h"

#include <string>

#include "hidclass.h"

#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"

namespace Common
{
namespace
{
// Shared by the devnode and interface variants, which take the same arguments apart from the
// first. Queries the size, then the value.
template <typename Getter, typename Target>
std::optional<std::wstring> ReadStringProperty(Getter getter, Target target,
                                               const DEVPROPKEY* key)
{
  DEVPROPTYPE type{};
  ULONG size = 0;
  const CONFIGRET probe = getter(target, key, &type, nullptr, &size, 0);
  if (probe != CR_SUCCESS && probe != CR_BUFFER_SMALL)
  {
    if (probe != CR_NO_SUCH_VALUE)
      WARN_LOG_FMT(COMMON, "Reading a device property failed: {}", probe);
    return std::nullopt;
  }
  if (type != DEVPROP_TYPE_STRING)
  {
    WARN_LOG_FMT(COMMON, "Device property has type 0x{:x}, expected a string", type);
    return std::nullopt;
  }
  if (size < sizeof(WCHAR))
    return std::wstring{};

  // `size` counts the terminator; std::wstring keeps room for it past size().
  std::wstring value(size / sizeof(WCHAR) - 1, L'\0');
  const CONFIGRET read =
      getter(target, key, &type, reinterpret_cast<BYTE*>(value.data()), &size, 0);
  if (read != CR_SUCCESS)
  {
    ERROR_LOG_FMT(COMMON, "Reading a device property failed: {}", read);
    return std::nullopt;
  }
  return value;
}

DWORD CALLBACK OnDeviceInterfaceChange(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                       PCM_NOTIFY_EVENT_DATA, DWORD)
{
  auto& callback = *static_cast<DeviceChangeNotification::CallbackType*>(context);
  if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL)
    callback(DeviceChangeNotification::EventType::Arrival);
  else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
    callback(DeviceChangeNotification::EventType::Removal);
  return ERROR_SUCCESS;
}
}  // namespace

std::optional<std::wstring> GetDevNodeStringProperty(DEVINST device,
                                                     const DEVPROPKEY* requested_property)
{
  return ReadStringProperty(CM_Get_DevNode_Property, device, requested_property);
}

std::optional<std::wstring> GetDeviceInterfaceStringProperty(LPCWSTR iface,
                                                             const DEVPROPKEY* requested_property)
{
  return ReadStringProperty(CM_Get_Device_Interface_Property, iface, requested_property);
}

NullTerminatedStringList<WCHAR> GetDeviceInterfaceList(LPGUID iface_class_guid, DEVINSTID device_id,
                                                       ULONG flags)
{
  // The list can grow between asking for its size and reading it; retry until it fits.
  for (;;)
  {
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_Size(&length, iface_class_guid, device_id, flags) !=
            CR_SUCCESS ||
        length == 0)
    {
      length = 1;
    }

    auto buffer = std::make_unique_for_overwrite<WCHAR[]>(length);
    const CONFIGRET result =
        CM_Get_Device_Interface_List(iface_class_guid, device_id, buffer.get(), length, flags);
    if (result == CR_BUFFER_SMALL)
      continue;

    if (result != CR_SUCCESS)
    {
      ERROR_LOG_FMT(COMMON, "Listing device interfaces failed: {}", result);
      buffer[0] = L'\0';
    }
    return {std::move(buffer)};
  }
}

DeviceChangeNotification::DeviceChangeNotification() = default;

DeviceChangeNotification::~DeviceChangeNotification()
{
  Unregister();
}

void DeviceChangeNotification::Register(CallbackType callback)
{
  Unregister();
  m_callback = std::move(callback);

  CM_NOTIFY_FILTER filter{};
  filter.cbSize = sizeof(filter);
  filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
  filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_HID;

  const CONFIGRET result =
      CM_Register_Notification(&filter, &m_callback, OnDeviceInterfaceChange, &m_notify_handle);
  if (result != CR_SUCCESS)
  {
    ERROR_LOG_FMT(COMMON, "Registering for device notifications failed: {:x}", result);
    m_notify_handle = nullptr;
  }
}

void DeviceChangeNotification::Unregister()
{
  if (m_notify_handle == nullptr)
    return;

  const CONFIGRET result = CM_Unregister_Notification(m_notify_handle);
  if (result != CR_SUCCESS)
    ERROR_LOG_FMT(COMMON, "Unregistering device notifications failed: {:x}", result);
  m_notify_handle = nullptr;
}

}  // namespace Common

#endif
