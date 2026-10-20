// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace Dolphin_Debugger
{
struct CallstackEntry
{
  std::string Name;
  u32 vAddress = 0;
};

bool GetCallstack(const Core::CPUThreadGuard& guard, std::vector<CallstackEntry>& output);
void PrintCallstack(const Core::CPUThreadGuard& guard, Common::Log::LogType type,
                    Common::Log::LogLevel level);
void PrintDataBuffer(const Core::System& system, Common::Log::LogType type, u32 address, u32 size,
                     std::string_view title);
}  // namespace Dolphin_Debugger
