// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DFloat.cpp

#include "R3DFloat.h"

#include <cstring>

namespace R3DFloat
{
  uint32_t ToIEEEBits(uint32_t r3d)
  {
    const uint32_t sign = r3d & 0x80000000u;
    const uint32_t exp6 = (r3d >> 25) & 0x3F;
    const uint32_t mantissa = (r3d >> 2) & 0x7FFFFF;
    // 6-bit two's complement exponent, rebased to IEEE's bias of 127.
    const uint32_t exponent = (exp6 < 32) ? exp6 + 127 : exp6 + 127 - 64;
    return sign | (exponent << 23) | mantissa;
  }

  float GetFloat32(uint32_t r3d)
  {
    uint32_t bits = ToIEEEBits(r3d);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

  float GetFloat16(uint16_t r3d)
  {
    return GetFloat32(uint32_t(r3d) << 15);
  }
}
