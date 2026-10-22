// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <unordered_set>

#include <windows.h>

namespace ciface::DInput
{
// The XInput controllers present, each as MAKELONG(vendor id, product id) (the Data1 of a
// DirectInput product GUID), so that DirectInput can leave them to the XInput backend.
std::unordered_set<DWORD> GetXInputGUIDS();
}  // namespace ciface::DInput
