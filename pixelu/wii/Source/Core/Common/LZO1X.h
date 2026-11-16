// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Reader and writer for the LZO1X compressed format, written from the published description
// of the bitstream. Old save states and the netplay data transfer use this format, so files
// and peers from earlier builds keep working.

#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace Common::LZO1X
{
// Largest compressed size Compress can produce for `length` input bytes. A literal run of 19 or
// more bytes costs two extra bytes and the shortest reference after it saves one, so the worst
// case grows by under one byte in 23; this is the same bound earlier builds allocated.
constexpr std::size_t WorstCase(std::size_t length)
{
  return length + length / 16 + 64 + 3;
}

// Compresses `length` bytes into `out` (at least WorstCase(length) bytes). Returns the
// compressed size.
std::size_t Compress(const u8* in, std::size_t length, u8* out);

// Decompresses a complete stream into at most `capacity` bytes. Returns false on a damaged or
// truncated stream or one that does not fit; `*out_length` is the number of bytes written.
bool Decompress(const u8* in, std::size_t length, u8* out, std::size_t capacity,
                std::size_t* out_length);
}  // namespace Common::LZO1X
