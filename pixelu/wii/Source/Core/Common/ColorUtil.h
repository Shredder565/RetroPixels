// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

// Decoders for the small tiled GameCube images found in save banners and icons.
// Output pixels are 0xAARRGGBB; translucent texels are composited onto black and made opaque.
namespace Common
{
// RGB5A3 texels in 4x4 tiles, big endian.
void Decode5A3Image(u32* dst, const u16* src, int width, int height);
// 8-bit palette indices in 8x4 tiles with an RGB5A3 palette (big endian).
void DecodeCI8Image(u32* dst, const u8* src, const u16* pal, int width, int height);
}  // namespace Common
