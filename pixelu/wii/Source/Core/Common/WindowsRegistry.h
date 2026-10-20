// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <windows.h>
#include <string>

#include "Common/CommonTypes.h"

namespace WindowsRegistry
{
// Reads HKEY_LOCAL_MACHINE\<subkey>\<name> into *value. Integers must be 32-bit (REG_DWORD) or
// 64-bit (REG_QWORD); strings are REG_SZ, read without expanding variables. Returns false if the
// value is missing or has another type.
template <typename T>
bool ReadValue(T* value, const std::string& subkey, const std::string& name);
template <>
bool ReadValue(std::string* value, const std::string& subkey, const std::string& name);

// The real Windows version: from the registry, which compatibility modes do not fake, falling
// back to RtlGetVersion.
OSVERSIONINFOW GetOSVersion();
}  // namespace WindowsRegistry
