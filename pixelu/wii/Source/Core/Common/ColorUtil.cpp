// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/ColorUtil.h"

#include "Common/Swap.h"

namespace Common
{
namespace
{
// Scale an n-bit channel to 0..255, rounding down.
constexpr u32 Expand3(u32 v)
{
  return v * 255 / 7;
}
constexpr u32 Expand4(u32 v)
{
  return v * 255 / 15;
}
constexpr u32 Expand5(u32 v)
{
  return v * 255 / 31;
}

// RGB5A3: bit 15 set means RGB555 (opaque); clear means ARGB3444.
u32 ConvertRGB5A3(u16 texel)
{
  u32 r, g, b;
  if (texel & 0x8000)
  {
    r = Expand5((texel >> 10) & 0x1F);
    g = Expand5((texel >> 5) & 0x1F);
    b = Expand5(texel & 0x1F);
  }
  else
  {
    // Premultiply onto a black background; the result is shown fully opaque.
    const u32 alpha = Expand3((texel >> 12) & 0x7);
    r = Expand4((texel >> 8) & 0xF) * alpha / 255;
    g = Expand4((texel >> 4) & 0xF) * alpha / 255;
    b = Expand4(texel & 0xF) * alpha / 255;
  }
  return 0xFF000000u | (r << 16) | (g << 8) | b;
}
}  // namespace

void Decode5A3Image(u32* dst, const u16* src, int width, int height)
{
  constexpr int TILE_W = 4;
  constexpr int TILE_H = 4;
  for (int tile_y = 0; tile_y < height; tile_y += TILE_H)
  {
    for (int tile_x = 0; tile_x < width; tile_x += TILE_W)
    {
      for (int row = 0; row < TILE_H; ++row)
      {
        u32* out = dst + (tile_y + row) * width + tile_x;
        for (int col = 0; col < TILE_W; ++col)
          out[col] = ConvertRGB5A3(Common::swap16(*src++));
      }
    }
  }
}

void DecodeCI8Image(u32* dst, const u8* src, const u16* pal, int width, int height)
{
  constexpr int TILE_W = 8;
  constexpr int TILE_H = 4;
  for (int tile_y = 0; tile_y < height; tile_y += TILE_H)
  {
    for (int tile_x = 0; tile_x < width; tile_x += TILE_W)
    {
      for (int row = 0; row < TILE_H; ++row)
      {
        u32* out = dst + (tile_y + row) * width + tile_x;
        for (int col = 0; col < TILE_W; ++col)
          out[col] = ConvertRGB5A3(Common::swap16(pal[*src++]));
      }
    }
  }
}
}  // namespace Common
