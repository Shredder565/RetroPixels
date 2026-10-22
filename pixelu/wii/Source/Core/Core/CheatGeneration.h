// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <expected>

#include "Core/ActionReplay.h"

namespace Cheats
{
class CheatSearchSessionBase;

enum class GenerateActionReplayCodeErrorCode
{
  IndexOutOfRange,
  NotVirtualMemory,
  InvalidAddress,
};

// Turns cheat search result `index` into an AR code that writes the result's current value.
std::expected<ActionReplay::ARCode, GenerateActionReplayCodeErrorCode>
GenerateActionReplayCode(const Cheats::CheatSearchSessionBase& session, size_t index);
}  // namespace Cheats
