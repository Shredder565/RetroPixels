// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "InputCommon/ControllerInterface/InputBackend.h"

// The Windows input backend: DirectInput, XInput and Windows.Gaming.Input devices, refreshed
// when HID devices come and go.
namespace ciface::Win32
{
std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);
}  // namespace ciface::Win32
