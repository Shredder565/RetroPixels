// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

namespace Config
{
struct Location;
}

namespace ConfigLoaders
{
// Whether the config system may write a setting to its INI file. Controller settings are
// written by the input configuration code instead, so the two don't overwrite each other.
bool IsSettingSaveable(const Config::Location& config_location);
}  // namespace ConfigLoaders
