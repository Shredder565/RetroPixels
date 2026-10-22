// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Game controllers through Windows.Gaming.Input (the Gamepad and RawGameController APIs),
// with their own hotplug notifications. Used by the Win32 backend.
namespace ciface::WGInput
{
// Initialises WinRT on this thread if needed and starts listening for controller changes.
void Init();
void DeInit();
// Replaces the devices of all connected controllers.
void PopulateDevices();
}  // namespace ciface::WGInput
