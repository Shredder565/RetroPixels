// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControllerInterface/DInput/XInputFilter.h"

#include <cwchar>
#include <optional>
#include <unordered_set>
#include <vector>

#include <windows.h>
#include <setupapi.h>

namespace ciface::DInput
{
namespace
{
// The "HIDClass" device setup class.
constexpr GUID HID_DEVICE_CLASS = {
    0x745a17a0, 0x74d3, 0x11d0, {0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda}};

// Reads the device's hardware IDs (a list of strings, each NUL terminated, ended by an empty
// one) into `ids`, growing it when the API asks for more room. Returns false on failure.
//
// Note that the buffer's element count is passed where the API expects a byte count, and the
// byte count it asks for becomes the new element count. The buffer thus ends up twice as large
// as needed, which is harmless.
bool ReadHardwareIds(HDEVINFO devices, SP_DEVINFO_DATA* device, std::vector<wchar_t>* ids)
{
  DWORD wanted = 0;
  while (!SetupDiGetDeviceRegistryPropertyW(devices, device, SPDRP_HARDWAREID, nullptr,
                                            reinterpret_cast<BYTE*>(ids->data()),
                                            static_cast<DWORD>(ids->size()), &wanted))
  {
    if (wanted <= ids->size())
      break;
    ids->resize(wanted);
  }
  return GetLastError() == ERROR_SUCCESS;
}

// XInput devices have "IG_" in a hardware ID; their vendor and product IDs come from the same
// string ("VID_xxxx", "PID_xxxx").
std::optional<DWORD> XInputIdFromHardwareId(const wchar_t* hardware_id)
{
  if (std::wcsstr(hardware_id, L"IG_") == nullptr)
    return std::nullopt;

  unsigned int vendor = 0;
  const wchar_t* const vendor_text = std::wcsstr(hardware_id, L"VID_");
  if (vendor_text == nullptr || std::swscanf(vendor_text, L"VID_%4X", &vendor) == 0)
    return std::nullopt;

  unsigned int product = 0;
  const wchar_t* const product_text = std::wcsstr(hardware_id, L"PID_");
  if (product_text == nullptr || std::swscanf(product_text, L"PID_%4X", &product) == 0)
    return std::nullopt;

  return static_cast<DWORD>(MAKELONG(vendor, product));
}
}  // namespace

// Looks through the hardware IDs of the present HID devices. This is much faster than the
// WMI query Microsoft suggests, which can take seconds when the system and user languages
// differ.
std::unordered_set<DWORD> GetXInputGUIDS()
{
  std::unordered_set<DWORD> found;

  const HDEVINFO devices = SetupDiGetClassDevsW(&HID_DEVICE_CLASS, nullptr, nullptr, DIGCF_PRESENT);
  if (devices == INVALID_HANDLE_VALUE)
    return found;

  std::vector<wchar_t> ids(128);
  SP_DEVINFO_DATA device;
  device.cbSize = sizeof(device);
  for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &device); ++index)
  {
    if (!ReadHardwareIds(devices, &device, &ids))
      continue;

    // Sub-devices of one controller report the same IDs; the set keeps one copy.
    for (std::size_t at = 0; ids[at] != L'\0'; at += std::wcslen(&ids[at]) + 1)
    {
      if (const std::optional<DWORD> id = XInputIdFromHardwareId(&ids[at]))
      {
        found.insert(*id);
        break;
      }
    }
  }

  SetupDiDestroyDeviceInfoList(devices);
  return found;
}
}  // namespace ciface::DInput
