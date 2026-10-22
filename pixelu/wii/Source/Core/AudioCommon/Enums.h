// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

namespace AudioCommon
{
// Quality of the Dolby Pro Logic II decoder; stored as an int in the config.
enum class DPL2Quality : int
{
  Lowest = 0,
  Low = 1,
  High = 2,
  Highest = 3
};
}  // namespace AudioCommon
