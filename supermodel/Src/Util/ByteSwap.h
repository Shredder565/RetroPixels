// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// In-place byte order reversal of buffers of 16- or 32-bit words (ROM images
// are stored big-endian on the Model 3's PowerPC side).

#ifndef INCLUDED_BYTESWAP_H
#define INCLUDED_BYTESWAP_H

#include <cstddef>
#include <cstdint>

namespace Util
{
  // Swaps the two bytes of every whole 16-bit word in the first 'size' bytes
  // (a trailing odd byte is left alone).
  void FlipEndian16(uint8_t *buffer, size_t size);

  // Reverses the four bytes of every whole 32-bit word in the first 'size'
  // bytes (up to three trailing bytes are left alone).
  void FlipEndian32(uint8_t *buffer, size_t size);
} // Util

#endif  // INCLUDED_BYTESWAP_H
