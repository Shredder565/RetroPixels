// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/Win32/Win32.h"

#include <windows.h>
#include <cfgmgr32.h>
// Defines (rather than declares) the GUIDs of the next header.
#include <initguid.h>

#include <hidclass.h>

#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DInput/DInput.h"
#include "InputCommon/ControllerInterface/WGInput/WGInput.h"
#include "InputCommon/ControllerInterface/XInput/XInput.h"

#pragma comment(lib, "OneCoreUAP.Lib")

namespace ciface::Win32
{
namespace
{
// Device notifications start arriving as soon as they are registered, before the first
// population; until then they are ignored.
Common::Flag s_populated;
HCMNOTIFICATION s_hid_notification;

class InputBackend final : public ciface::InputBackend
{
public:
  explicit InputBackend(ControllerInterface* controller_interface);
  ~InputBackend() override;

  void PopulateDevices() override;
  void HandleWindowChange() override;

  // On a HID device arrival or removal.
  void OnHidDeviceChange();

private:
  HWND RenderWindow();
};

DWORD CALLBACK OnHidNotification(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                 PCM_NOTIFY_EVENT_DATA, DWORD)
{
  if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ||
      action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
  {
    static_cast<InputBackend*>(context)->OnHidDeviceChange();
  }
  return ERROR_SUCCESS;
}

InputBackend::InputBackend(ControllerInterface* controller_interface)
    : ciface::InputBackend(controller_interface)
{
  XInput::Init();
  WGInput::Init();

  CM_NOTIFY_FILTER filter{};
  filter.cbSize = sizeof(filter);
  filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
  filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_HID;
  const CONFIGRET result =
      CM_Register_Notification(&filter, this, OnHidNotification, &s_hid_notification);
  if (result != CR_SUCCESS)
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "CM_Register_Notification failed: {:x}", result);
}

InputBackend::~InputBackend()
{
  s_populated.Clear();
  DInput::DeInit();

  if (s_hid_notification)
  {
    const CONFIGRET result = CM_Unregister_Notification(s_hid_notification);
    if (result != CR_SUCCESS)
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "CM_Unregister_Notification failed: {:x}", result);
    s_hid_notification = nullptr;
  }

  XInput::DeInit();
  WGInput::DeInit();
}

HWND InputBackend::RenderWindow()
{
  return static_cast<HWND>(GetControllerInterface().GetWindowSystemInfo().render_window);
}

void InputBackend::PopulateDevices()
{
  g_controller_interface.PlatformPopulateDevices([this] {
    s_populated.Set();
    DInput::PopulateDevices(RenderWindow());
    XInput::PopulateDevices();
    WGInput::PopulateDevices();
  });
}

void InputBackend::OnHidDeviceChange()
{
  if (!s_populated.IsSet())
    return;

  // Windows.Gaming.Input reports its own device changes.
  g_controller_interface.PlatformPopulateDevices([this] {
    DInput::PopulateDevices(RenderWindow());
    XInput::PopulateDevices();
  });
}

void InputBackend::HandleWindowChange()
{
  g_controller_interface.PlatformPopulateDevices([this] { DInput::ChangeWindow(RenderWindow()); });
}
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}
}  // namespace ciface::Win32
