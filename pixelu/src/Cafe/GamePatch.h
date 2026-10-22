// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// applies the built-in fixes for individual games to the loaded code
void GamePatch_scan();
// true for HLE hooks that continue after the hooked instruction instead of returning
bool GamePatch_IsNonReturnFunction(uint32 hleIndex);
