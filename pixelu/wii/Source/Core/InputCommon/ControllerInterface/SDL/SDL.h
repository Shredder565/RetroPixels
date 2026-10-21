// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "InputCommon/ControllerInterface/InputBackend.h"

// Joysticks and gamepads through SDL, with hotplug handled on SDL's own event thread.
namespace ciface::SDL
{
std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);
}  // namespace ciface::SDL
