// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>
#include <bit>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"

#include "VideoCommon/BPMemory.h"

// Size of the embedded framebuffer in pixels (without the multisampling modes).
constexpr u32 EFB_WIDTH = 640u;
constexpr u32 EFB_HEIGHT = 528u;

// Largest depth value the emulated GPU writes. Depth is stored as a fraction of 2^24 so that it
// survives the round trip through float exactly; the real hardware tops out at 2^24 - 1.
constexpr float MAX_EFB_DEPTH = 16777215.0f / 16777216.0f;

// Largest XFB the video interface displays. EFB copies are at most 640 wide, so wider XFBs are
// assembled from several copies; taller ones come from vertically scaled copies.
constexpr u32 MAX_XFB_WIDTH = 720u;
constexpr u32 MAX_XFB_HEIGHT = 576u;

#define PRIM_LOG(t, ...) DEBUG_LOG_FMT(VIDEO, t __VA_OPT__(, ) __VA_ARGS__)

// Per-vertex debug logging hook; compiled out.
#define LOG_VTX()

// Host graphics API family, as the shader generators see it.
enum class APIType
{
  OpenGL,
  D3D,
  Vulkan,
  Metal,
  Nothing
};

// Rounds each channel of an RGBA8 value down to 6 bits and widens it back to 8 by repeating the
// top two bits, as the RGBA6 EFB format stores it.
inline u32 RGBA8ToRGBA6ToRGBA8(u32 src)
{
  const u32 six_bit = src & 0xFCFCFCFC;
  return six_bit | ((six_bit >> 6) & 0x03030303);
}

// Same for RGB565: red and blue keep 5 bits (top 3 repeated), green 6 bits (top 2 repeated);
// alpha becomes opaque and only the low 24 bits of src are used.
inline u32 RGBA8ToRGB565ToRGBA8(u32 src)
{
  const u32 red = (src >> 16) & 0xF8;
  const u32 green = (src >> 8) & 0xFC;
  const u32 blue = src & 0xF8;
  const u32 r8 = red | (red >> 5);
  const u32 g8 = green | (green >> 6);
  const u32 b8 = blue | (blue >> 5);
  return 0xFF000000 | (r8 << 16) | (g8 << 8) | b8;
}

// A 24-bit depth reduced to its top 16 bits, with the top byte repeated as the low byte.
inline u32 Z24ToZ16ToZ24(u32 src)
{
  return (src >> 16) | (src & 0x00FFFF00);
}

// Encodes a 24-bit depth value in one of the 16-bit Z formats of the RGB565_Z16 EFB mode.
// ZLINEAR keeps the top 16 bits. ZNEAR, ZMID and ZFAR are float-like formats with 2, 3 or 4
// exponent bits: the exponent counts the leading one bits (capped at 3, 7 or 12), the zero
// ending the run is implied and dropped, and the mantissa holds the bits after it. At the cap
// the run may go on, so no bit is dropped there.
inline u32 CompressZ16(u32 z24depth, DepthFormat format)
{
  u32 exponent_bits;
  u32 exponent_cap;
  switch (format)
  {
  case DepthFormat::ZNEAR:
    exponent_bits = 2;
    exponent_cap = 3;
    break;
  case DepthFormat::ZMID:
    exponent_bits = 3;
    exponent_cap = 7;
    break;
  case DepthFormat::ZFAR:
    exponent_bits = 4;
    exponent_cap = 12;
    break;
  default:
    // ZLINEAR, and the inverted formats the hardware does not seem to support.
    return z24depth >> 8;
  }

  const u32 mantissa_bits = 16 - exponent_bits;
  const u32 run = static_cast<u32>(std::countl_one(z24depth << 8));
  const u32 exponent = std::min(run, exponent_cap);
  const u32 skipped = run < exponent_cap ? exponent + 1 : exponent;
  const u32 shift = 24 - skipped - mantissa_bits;
  const u32 mantissa = (z24depth >> shift) & ((1u << mantissa_bits) - 1);
  return (exponent << mantissa_bits) | mantissa;
}
