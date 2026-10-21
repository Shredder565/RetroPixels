// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

namespace Core
{
class System;
}
struct BootParameters;
struct WindowSystemInfo;

namespace BootManager
{
// Starts emulation of `parameters`: works out the game's metadata and the
// session's settings, prepares the Wii NAND root, then hands over to Core::Init.
bool BootCore(Core::System& system, std::unique_ptr<BootParameters> parameters,
              const WindowSystemInfo& wsi);

// Undoes the session's configuration when emulation ends: takes back the
// SYSCONF settings the guest may have changed, tears down the Wii root and
// drops the per-game, movie and NetPlay config layers.
void RestoreConfig();
}  // namespace BootManager
