// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <ctime>
#include <optional>

namespace Common
{
// Converts `time` to the local calendar time without std::localtime's shared static buffer.
// Returns nullopt (and logs) if the conversion fails.
std::optional<std::tm> LocalTime(std::time_t time);
}  // namespace Common
