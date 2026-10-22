// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

namespace Core
{
class System;
}

namespace IOS::HLE::MIOS
{
// Switches the console into GameCube mode and runs the GameCube IPL until it is ready.
bool Load(Core::System& system);
}  // namespace IOS::HLE::MIOS
