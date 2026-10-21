// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/DInput/DInput.h"

#include "Common/HRWrap.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/DInput/DInputJoystick.h"
#include "InputCommon/ControllerInterface/DInput/DInputKeyboardMouse.h"

#pragma comment(lib, "Dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace ciface::DInput
{
namespace
{
// Created by the first PopulateDevices(), released by DeInit().
IDirectInput8* s_dinput = nullptr;

bool IsDInputDevice(const Core::Device* device)
{
  return device->GetSource() == DINPUT_SOURCE_NAME;
}
}  // namespace

BOOL CALLBACK DIEnumDeviceObjectsCallback(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef)
{
  static_cast<std::list<DIDEVICEOBJECTINSTANCE>*>(pvRef)->push_back(*lpddoi);
  return DIENUM_CONTINUE;
}

BOOL CALLBACK DIEnumDevicesCallback(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef)
{
  static_cast<std::list<DIDEVICEINSTANCE>*>(pvRef)->push_back(*lpddi);
  return DIENUM_CONTINUE;
}

std::string GetDeviceName(const LPDIRECTINPUTDEVICE8 device)
{
  DIPROPSTRING product{};
  product.diph.dwSize = sizeof(product);
  product.diph.dwHeaderSize = sizeof(product.diph);
  product.diph.dwHow = DIPH_DEVICE;

  const HRESULT result = device->GetProperty(DIPROP_PRODUCTNAME, &product.diph);
  if (FAILED(result))
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "GetProperty(DIPROP_PRODUCTNAME) failed: {}",
                  Common::HRWrap(result));
    return {};
  }
  return std::string(StripWhitespace(WStringToUTF8(product.wsz)));
}

void PopulateDevices(HWND hwnd)
{
  if (s_dinput == nullptr)
  {
    const HRESULT result =
        DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8,
                           reinterpret_cast<LPVOID*>(&s_dinput), nullptr);
    if (FAILED(result))
    {
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "DirectInput8Create failed: {}", Common::HRWrap(result));
      return;
    }
  }

  // Joysticks that can no longer be acquired are gone. The others stay, so a joystick may
  // keep a higher id than a full repopulation would give it. The keyboard and mouse always
  // stay.
  g_controller_interface.RemoveDevice(
      [](const Core::Device* device) { return IsDInputDevice(device) && !device->IsValid(); });

  InitKeyboardMouse(s_dinput, hwnd);
  InitJoystick(s_dinput, hwnd);
}

void ChangeWindow(HWND hwnd)
{
  // Nothing to do before the first PopulateDevices().
  if (s_dinput == nullptr)
    return;

  // The joysticks are bound to the old window and must be gone right away to be created again;
  // the keyboard and mouse (a virtual device) just follow the new window.
  g_controller_interface.RemoveDevice(
      [](const Core::Device* device) {
        return IsDInputDevice(device) && !device->IsVirtualDevice();
      },
      true);

  SetKeyboardMouseWindow(hwnd);
  InitJoystick(s_dinput, hwnd);
}

void DeInit()
{
  if (s_dinput == nullptr)
    return;

  s_dinput->Release();
  s_dinput = nullptr;
}
}  // namespace ciface::DInput
