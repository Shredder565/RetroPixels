// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DFloat.h
//
// The Real3D's own floating point formats, used for the culling and blend
// radii of culling nodes. A 32-bit value has a sign bit (31), a 6-bit two's
// complement exponent (30-25) and a 23-bit mantissa (24-2). The 16-bit form
// is always positive: it holds bits 30-15 of the 32-bit form (exponent and
// the top 10 mantissa bits).

#ifndef INCLUDED_NEW3D_R3DFLOAT_H
#define INCLUDED_NEW3D_R3DFLOAT_H

#include <cstdint>

namespace R3DFloat
{
  uint32_t ToIEEEBits(uint32_t r3d);          // 32-bit Real3D float -> IEEE 754 bit pattern
  float GetFloat32(uint32_t r3d);
  float GetFloat16(uint16_t r3d);
}

#endif  // INCLUDED_NEW3D_R3DFLOAT_H
