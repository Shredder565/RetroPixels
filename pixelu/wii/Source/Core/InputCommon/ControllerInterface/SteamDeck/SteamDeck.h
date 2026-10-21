// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "InputCommon/ControllerInterface/InputBackend.h"

// The Steam Deck's built-in controls, read directly over HID.
namespace ciface::SteamDeck
{
std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);
}
