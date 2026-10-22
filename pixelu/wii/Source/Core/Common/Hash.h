// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Common
{
u32 HashAdler32(const u8* data, size_t len);
// Legacy checksum; only for compatibility with values computed by older versions.
u32 HashEctor(const u8* data, size_t len);

// Fast 64-bit hash for the texture cache. With samples != 0, only about that many 8-byte words
// spread over the data are hashed (plus the tail). The result depends on the CPU (CRC32
// instructions or not), so it must not be stored.
u64 GetHash64(const u8* src, u32 len, u32 samples);

// zlib-compatible CRC-32.
u32 StartCRC32();
u32 UpdateCRC32(u32 crc, const u8* data, size_t len);
u32 ComputeCRC32(const u8* data, size_t len);
u32 ComputeCRC32(std::string_view data);
}  // namespace Common
