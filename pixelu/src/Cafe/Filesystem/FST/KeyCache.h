// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Disc keys from keys.txt in the user data folder (created with an example on first use).

void KeyCache_Prepare(); // loads keys.txt once

uint8* KeyCache_GetAES128(sint32 index); // nullptr past the last key
