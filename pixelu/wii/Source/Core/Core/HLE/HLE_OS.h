// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace HLE_OS
{
// Source of the values consumed while expanding a printf-style format.
class HLEPrintArgs
{
public:
  virtual u32 GetU32() = 0;
  virtual u64 GetU64() = 0;
  virtual double GetF64() = 0;
  virtual std::string GetString(std::optional<u32> max_length) = 0;
  virtual std::u16string GetU16String(std::optional<u32> max_length) = 0;
};

// Expands a guest printf-style format string, pulling arguments from `args`.
std::string GetStringVA(HLEPrintArgs* args, std::string_view string);

void HLE_GeneralDebugPrint(const Core::CPUThreadGuard& guard);
void HLE_GeneralDebugVPrint(const Core::CPUThreadGuard& guard);
void HLE_write_console(const Core::CPUThreadGuard& guard);
void HLE_OSPanic(const Core::CPUThreadGuard& guard);
void HLE_LogDPrint(const Core::CPUThreadGuard& guard);
void HLE_LogVDPrint(const Core::CPUThreadGuard& guard);
void HLE_LogFPrint(const Core::CPUThreadGuard& guard);
void HLE_LogVFPrint(const Core::CPUThreadGuard& guard);
}  // namespace HLE_OS
