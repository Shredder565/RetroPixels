// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Clears the BP register file to its power-on state.
void BPInit();
// Re-applies the current BP registers to the backend (after a savestate load).
void BPReload();
