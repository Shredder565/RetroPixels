// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "Util/ByteSwap.h"
#include <utility>

namespace Util
{
  void FlipEndian16(uint8_t *buffer, size_t size)
  {
    uint8_t *end = buffer + (size & ~size_t(1));
    for (uint8_t *p = buffer; p != end; p += 2)
      std::swap(p[0], p[1]);
  }

  void FlipEndian32(uint8_t *buffer, size_t size)
  {
    uint8_t *end = buffer + (size & ~size_t(3));
    for (uint8_t *p = buffer; p != end; p += 4)
    {
      std::swap(p[0], p[3]);
      std::swap(p[1], p[2]);
    }
  }
} // Util
