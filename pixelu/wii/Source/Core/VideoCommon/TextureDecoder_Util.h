// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

#include "VideoCommon/LookUpTables.h"

// Helpers shared by the texture decoders. Decoded texels are packed little-endian RGBA8:
// red in bits 0-7, green 8-15, blue 16-23, alpha 24-31.

// One 4x4 CMPR sub-block as it sits in memory: two big-endian RGB565 endpoints followed by
// one byte of four 2-bit selectors per row (leftmost texel in the top bits).
struct DXTBlock
{
  u16 color1;
  u16 color2;
  u8 lines[4];
};

constexpr u32 MakeRGBA(int r, int g, int b, int a)
{
  return (static_cast<u32>(a) << 24) | (static_cast<u32>(b) << 16) | (static_cast<u32>(g) << 8) |
         static_cast<u32>(r);
}

// The CMPR interpolated colours weight the two endpoints 5:3 (not the 2:1 of PC DXT1).
// Returns (3 * near + 5 * far) / 8 rounded down, where `far` gets the larger weight.
constexpr int DXTBlend(int near_color, int far_color)
{
  return (near_color * 3 + far_color * 5) >> 3;
}

namespace TextureDecoderUtil
{
// IA8 as stored (intensity in the high byte of the big-endian value, which after a plain
// little-endian load sits in the low byte): `raw` is the value as loaded from memory.
constexpr u32 IA8FromRaw(u16 raw)
{
  const u32 alpha = raw & 0xFFu;
  const u32 intensity = static_cast<u32>(raw >> 8);
  return intensity * 0x00010101u | (alpha << 24);
}

// RGB565 from a big-endian value already swapped to host order.
constexpr u32 RGB565FromValue(u16 v)
{
  return MakeRGBA(Convert5To8(static_cast<u8>(v >> 11)), Convert6To8(static_cast<u8>((v >> 5) & 0x3F)),
                  Convert5To8(static_cast<u8>(v & 0x1F)), 0xFF);
}

// RGB5A3 from a host-order value: top bit set means opaque RGB555, clear means ARGB3444.
// (A branch: palettes and textures rarely mix the two forms, so it predicts well.)
constexpr u32 RGB5A3FromValue(u16 v)
{
  if (v & 0x8000)
  {
    return MakeRGBA(Convert5To8(static_cast<u8>((v >> 10) & 0x1F)),
                    Convert5To8(static_cast<u8>((v >> 5) & 0x1F)),
                    Convert5To8(static_cast<u8>(v & 0x1F)), 0xFF);
  }
  return MakeRGBA(Convert4To8(static_cast<u8>((v >> 8) & 0xF)),
                  Convert4To8(static_cast<u8>((v >> 4) & 0xF)), Convert4To8(static_cast<u8>(v & 0xF)),
                  Convert3To8(static_cast<u8>((v >> 12) & 0x7)));
}

constexpr u16 ByteSwap16(u16 v)
{
  return static_cast<u16>((v >> 8) | (v << 8));
}

// The four colours a CMPR sub-block can pick from, given its two endpoints as stored
// (big-endian). When the first endpoint is not greater than the second, entry 2 is the plain
// average and entry 3 is that same average with zero alpha.
//
// The arithmetic runs on all three channels at once: each channel gets its own 16-bit field
// of a u64 (red at bit 0, green at 16, blue at 32), wide enough that the weighted sums never
// carry into the next field. Bits shifted down from a neighbouring field are masked off.
namespace detail
{
constexpr u64 kFieldMask = 0x000000FF00FF00FFull;

constexpr u64 ExpandRGB565(u16 v)
{
  return static_cast<u64>(Convert5To8(static_cast<u8>(v >> 11))) |
         static_cast<u64>(Convert6To8(static_cast<u8>((v >> 5) & 0x3F))) << 16 |
         static_cast<u64>(Convert5To8(static_cast<u8>(v & 0x1F))) << 32;
}

constexpr u32 FieldsToRGB(u64 fields)
{
  return static_cast<u32>((fields & 0xFF) | ((fields >> 8) & 0xFF00) | ((fields >> 16) & 0xFF0000));
}
}  // namespace detail

inline std::array<u32, 4> CMPRPalette(u16 raw1, u16 raw2)
{
  const u16 c1 = ByteSwap16(raw1);
  const u16 c2 = ByteSwap16(raw2);
  const u64 e1 = detail::ExpandRGB565(c1);
  const u64 e2 = detail::ExpandRGB565(c2);

  const u32 toward1 = detail::FieldsToRGB(((e2 * 3 + e1 * 5) >> 3) & detail::kFieldMask);
  const u32 toward2 = detail::FieldsToRGB(((e1 * 3 + e2 * 5) >> 3) & detail::kFieldMask);
  const u32 average = detail::FieldsToRGB(((e1 + e2) >> 1) & detail::kFieldMask);

  // All ones when the endpoints are in "four opaque colours" order.
  const u32 four_colors = 0u - static_cast<u32>(c1 > c2);

  std::array<u32, 4> colors;
  colors[0] = detail::FieldsToRGB(e1) | 0xFF000000u;
  colors[1] = detail::FieldsToRGB(e2) | 0xFF000000u;
  colors[2] = ((toward1 & four_colors) | (average & ~four_colors)) | 0xFF000000u;
  colors[3] = (toward2 & four_colors) | ((average & ~four_colors)) | (four_colors & 0xFF000000u);
  return colors;
}
}  // namespace TextureDecoderUtil
