// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "DiscIO/Blob.h"

namespace DiscIO
{
// Wraps a reader so that a background thread copies the whole disc into memory; reads are
// served from memory once the part they need has arrived, and from the wrapped reader until
// then. Copies of the returned reader share the same memory copy.
std::unique_ptr<BlobReader> CreateCachedBlobReader(std::unique_ptr<BlobReader> reader);

// The same, but clusters DiscScrubber finds unused aren't copied (reads of them still work,
// through the wrapped reader).
std::unique_ptr<BlobReader> CreateScrubbingCachedBlobReader(std::unique_ptr<BlobReader> reader);

}  // namespace DiscIO
