// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Raw access to Wii U disc images: plain .wud files and .wux files (sector
// deduplicated .wud). Reads always address the uncompressed image.
struct wud_t;

wud_t* wud_open(const fs::path& path); // nullptr if the file cannot be read or has a bad .wux header
void wud_close(wud_t* wud);

bool wud_isWUXCompressed(wud_t* wud);
unsigned int wud_readData(wud_t* wud, void* buffer, unsigned int length, long long offset); // returns the number of bytes read
long long wud_getWUDSize(wud_t* wud); // size of the uncompressed image
