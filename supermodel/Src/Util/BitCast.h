// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Reinterpreting the bits of a 32-bit float as an integer and back. The
// Real3D display lists keep IEEE-754 single-precision values in plain 32-bit
// words, so the renderers read them with these.

#ifndef INCLUDED_UTIL_BITCAST_H
#define INCLUDED_UTIL_BITCAST_H

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Util
{
  // Copies the object representation of 'from' into a 'To' (both must be the
  // same size and trivially copyable). memcpy is the portable way to do this
  // before C++20 and compiles to a register move.
  template <typename To, typename From>
  inline To ReinterpretBits(const From &from)
  {
    static_assert(sizeof(To) == sizeof(From), "ReinterpretBits: sizes differ");
    static_assert(std::is_trivially_copyable<To>::value && std::is_trivially_copyable<From>::value,
                  "ReinterpretBits: types must be trivially copyable");
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
  }

  inline int32_t FloatAsInt32(float f)     { return ReinterpretBits<int32_t>(f); }
  inline uint32_t FloatAsUint32(float f)   { return ReinterpretBits<uint32_t>(f); }
  inline float Int32AsFloat(int32_t bits)  { return ReinterpretBits<float>(bits); }
  inline float Uint32AsFloat(uint32_t bits) { return ReinterpretBits<float>(bits); }
} // Util

#endif  // INCLUDED_UTIL_BITCAST_H
