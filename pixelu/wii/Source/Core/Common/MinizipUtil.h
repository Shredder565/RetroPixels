// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <algorithm>

#include <mz.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include "Common/CommonTypes.h"
#include "Common/ScopeGuard.h"

namespace Common
{
// Reads exactly `len` bytes of the zip reader's current entry into `destination`.
inline bool ReadFileFromZip(void* zip_reader, u8* destination, u64 len)
{
  if (mz_zip_reader_entry_open(zip_reader) != MZ_OK)
    return false;
  Common::ScopeGuard close_entry{[&] { mz_zip_reader_entry_close(zip_reader); }};

  constexpr u64 CHUNK_SIZE = 65535;  // fits the reader's int length parameter
  u64 remaining = len;
  while (remaining != 0)
  {
    const int chunk = static_cast<int>(std::min(remaining, CHUNK_SIZE));
    const int got = mz_zip_reader_entry_read(zip_reader, destination, chunk);
    // An error, or the entry ended before `len` bytes.
    if (got <= 0)
      return false;
    remaining -= static_cast<u64>(got);
    destination += got;
  }
  return true;
}

// Fills a contiguous container (sized by the caller) from the current entry.
template <typename ContiguousContainer>
bool ReadFileFromZip(void* file, ContiguousContainer* destination)
{
  return ReadFileFromZip(file, reinterpret_cast<u8*>(destination->data()), destination->size());
}
}  // namespace Common
