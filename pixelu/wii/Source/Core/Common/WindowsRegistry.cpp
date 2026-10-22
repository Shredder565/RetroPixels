// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/WindowsRegistry.h"

#include <windows.h>
#include <string>
#include <type_traits>

#include "Common/StringUtil.h"

namespace WindowsRegistry
{
namespace
{
LSTATUS GetMachineValue(const std::string& subkey, const std::string& name, DWORD flags,
                        void* data, DWORD* size)
{
  return RegGetValueA(HKEY_LOCAL_MACHINE, subkey.c_str(), name.c_str(), flags, nullptr, data,
                      size);
}
}  // namespace

template <typename T>
bool ReadValue(T* value, const std::string& subkey, const std::string& name)
{
  static_assert(std::is_integral_v<T> && (sizeof(T) == 4 || sizeof(T) == 8),
                "ReadValue supports 32- and 64-bit integers and std::string");
  constexpr DWORD type_flag = sizeof(T) == 4 ? RRF_RT_REG_DWORD : RRF_RT_REG_QWORD;
  DWORD size = sizeof(T);
  return GetMachineValue(subkey, name, type_flag, value, &size) == ERROR_SUCCESS;
}

template <>
bool ReadValue(std::string* value, const std::string& subkey, const std::string& name)
{
  constexpr DWORD flags = RRF_RT_REG_SZ | RRF_NOEXPAND;

  // Ask for the size (including the terminator), then read.
  DWORD size = 0;
  const LSTATUS probe = GetMachineValue(subkey, name, flags, nullptr, &size);
  if (probe != ERROR_SUCCESS && probe != ERROR_MORE_DATA)
    return false;

  value->resize(size);
  if (GetMachineValue(subkey, name, flags, value->data(), &size) != ERROR_SUCCESS)
  {
    value->clear();
    return false;
  }
  TruncateToCString(value);
  return true;
}

// Used from other translation units (the updater and CPU detection).
template bool ReadValue(u32* value, const std::string& subkey, const std::string& name);
template bool ReadValue(u64* value, const std::string& subkey, const std::string& name);

OSVERSIONINFOW GetOSVersion()
{
  constexpr char subkey[] = R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)";

  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);

  std::string build;
  const bool from_registry =
      ReadValue(&info.dwMajorVersion, subkey, "CurrentMajorVersionNumber") &&
      ReadValue(&info.dwMinorVersion, subkey, "CurrentMinorVersionNumber") &&
      ReadValue(&build, subkey, "CurrentBuildNumber") && TryParse(build, &info.dwBuildNumber);
  if (from_registry)
    return info;

  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll");
  const auto rtl_get_version =
      ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
  if (rtl_get_version)
    rtl_get_version(&info);

  // Keep only what the registry path would have provided.
  info.dwPlatformId = 0;
  info.szCSDVersion[0] = L'\0';
  return info;
}
}  // namespace WindowsRegistry
