// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// CRC-32 (zlib polynomial). Pass the previous result as c to continue a running checksum.
uint32 crc32_calc(uint32 c, const void* data, size_t length);

inline uint32 crc32_calc(const void* data, size_t length)
{
	return crc32_calc(0, data, length);
}
