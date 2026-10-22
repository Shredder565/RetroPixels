// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

// Expansion of an N-bit colour channel to 8 bits. The source bits are repeated from the top
// down until all eight output bits are filled, so 0 maps to 0 and the all-ones value maps to 255.
namespace LookUpTablesDetail
{
template <unsigned Bits>
constexpr u8 ExpandToByte(u8 value)
{
  unsigned out = 0;
  int shift = 8 - static_cast<int>(Bits);
  // Lay copies of the value down at decreasing shifts; the last copy may be cut off at the bottom.
  while (shift > -static_cast<int>(Bits))
  {
    out |= shift >= 0 ? (static_cast<unsigned>(value) << shift) :
                        (static_cast<unsigned>(value) >> -shift);
    shift -= static_cast<int>(Bits);
  }
  return static_cast<u8>(out & 0xFFu);
}
}  // namespace LookUpTablesDetail

constexpr u8 Convert3To8(u8 v)
{
  return LookUpTablesDetail::ExpandToByte<3>(v);
}

constexpr u8 Convert4To8(u8 v)
{
  return LookUpTablesDetail::ExpandToByte<4>(v);
}

constexpr u8 Convert5To8(u8 v)
{
  return LookUpTablesDetail::ExpandToByte<5>(v);
}

constexpr u8 Convert6To8(u8 v)
{
  return LookUpTablesDetail::ExpandToByte<6>(v);
}
