// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// First start: without a controller profile for player 1 nothing can be played.
// If neither controllerProfiles/controller0.xml nor the older controller0.txt
// exists, this saves one: an emulated GamePad on the first game controller
// (XInput for Xbox style pads, otherwise SDL) plus keyboard keys. An existing
// profile is left as it is. Call before InputManager::load().
void DefaultControllerProfile_CreateIfMissing();
