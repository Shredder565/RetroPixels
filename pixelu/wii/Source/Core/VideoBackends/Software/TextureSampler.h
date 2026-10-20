// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

// Texture lookups for the TEV: mip selection, coordinate wrapping and bilinear filtering of the
// texture bound to a texture map, decoded from main memory or TMEM.
namespace TextureSampler
{
// s and t are s17.7 texel coordinates and lod is s28.4. The result is four bytes in the order
// R, G, B, A.
void Sample(s32 s, s32 t, s32 lod, bool linear, u8 texmap, u8* sample);

// The same lookup at a fixed mip level.
void SampleMip(s32 s, s32 t, s32 mip, bool linear, u8 texmap, u8* sample);
}  // namespace TextureSampler
