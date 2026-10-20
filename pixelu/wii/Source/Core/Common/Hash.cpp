// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Hash.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <zlib.h>

#include "Common/CPUDetect.h"
#include "Common/Intrinsics.h"

namespace Common
{
u32 HashAdler32(const u8* data, size_t len)
{
  return adler32_z(1, data, len);
}

// A weak rotate-and-xor checksum. Kept only because existing stored values depend on it.
u32 HashEctor(const u8* data, size_t len)
{
  u32 hash = 0;
  for (size_t i = 0; i < len; ++i)
    hash = std::rotl(hash ^ data[i], 3);
  return hash;
}

namespace
{
u64 LoadU64(const u8* p)
{
  u64 value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

// Up to 8 trailing bytes as a little-endian integer (missing bytes are zero).
u64 LoadPartialU64(const u8* p, u32 count)
{
  u64 value = 0;
  std::memcpy(&value, p, count);
  return value;
}

// Distance between sampled 8-byte words: `words / samples`, where samples == 0 means "all", and
// at least 1.
u32 SampleStride(u32 words, u32 samples)
{
  if (samples == 0)
    samples = std::max(words, 1u);
  return std::max(words / samples, 1u);
}

// --- Texture hash without CRC32 instructions --------------------------------------------------
// A MurmurHash3-x64-128-style mix over (a sample of) the 16-byte blocks, returning the low half.
// The block mixing uses evolving multipliers, so it is NOT the reference MurmurHash3 output; the
// constants below define the hash values texture caches have been keyed by.

struct MurmurState
{
  u64 h1 = 0x9368e53c2f6af274;
  u64 h2 = 0x586dcd208f7cd3fd;
  u64 c1 = 0x87c37b91114253d5;
  u64 c2 = 0x4cf5ad432745937f;

  void Mix(u64 k1, u64 k2)
  {
    h1 ^= std::rotl(k1 * c1, 23) * c2;
    h1 += h2;
    h2 = std::rotl(h2, 41);
    h2 ^= std::rotl(k2 * c2, 23) * c1;
    h2 += h1;

    h1 = h1 * 3 + 0x52dce729;
    h2 = h2 * 3 + 0x38495ab5;
    c1 = c1 * 5 + 0x7b7d159c;
    c2 = c2 * 5 + 0x6bce6396;
  }
};

u64 Avalanche64(u64 k)
{
  k = (k ^ (k >> 33)) * 0xff51afd7ed558ccd;
  k = (k ^ (k >> 33)) * 0xc4ceb9fe1a85ec53;
  return k ^ (k >> 33);
}

u64 TextureHashGeneric(const u8* src, u32 len, u32 samples)
{
  const u32 block_count = len / 16;
  const u32 stride = SampleStride(len / 8, samples);

  MurmurState state;
  for (u32 block = 0; block < block_count; block += stride)
    state.Mix(LoadU64(src + block * 16), LoadU64(src + block * 16 + 8));

  if (const u32 rest = len % 16; rest != 0)
  {
    const u8* tail = src + block_count * 16;
    const u64 k1 = LoadPartialU64(tail, std::min(rest, 8u));
    const u64 k2 = rest > 8 ? LoadPartialU64(tail + 8, rest - 8) : 0;
    state.Mix(k1, k2);
  }

  u64 h1 = state.h1;
  u64 h2 = state.h2 ^ len;
  h1 += h2;
  h2 += h1;
  h1 = Avalanche64(h1);
  h2 = Avalanche64(h2);
  return h1 + h2;
}

// --- Texture hash with SSE4.2 CRC32 ---------------------------------------------------------
// Four CRC32C lanes, each fed every fourth sampled 8-byte word; lane 0 starts from the length
// and also takes the trailing bytes. The lanes are combined with shifted adds.

FUNCTION_TARGET_SSE42
u64 TextureHashCRC32(const u8* src, u32 len, u32 samples)
{
  const u32 words = len / 8;
  const u32 stride = SampleStride(words, samples);
  u64 lane[4] = {len, 0, 0, 0};

  // Word indices are computed in 64 bits: idx + 3 * stride must not wrap.
  u64 idx = 0;
  for (; idx + 3 * u64{stride} < words; idx += 4 * u64{stride})
  {
    for (int l = 0; l < 4; ++l)
      lane[l] = _mm_crc32_u64(lane[l], LoadU64(src + (idx + l * u64{stride}) * 8));
  }
  // Fewer than four sampled words left.
  for (int l = 0; l < 3; ++l)
  {
    if (idx + l * u64{stride} < words)
      lane[l] = _mm_crc32_u64(lane[l], LoadU64(src + (idx + l * u64{stride}) * 8));
  }

  if (const u32 rest = len % 8; rest != 0)
    lane[0] = _mm_crc32_u64(lane[0], LoadPartialU64(src + u64{words} * 8, rest));

  return lane[0] + (lane[1] << 10) + (lane[2] << 21) + (lane[3] << 32);
}

using TextureHashFn = u64 (*)(const u8*, u32, u32);

TextureHashFn PickTextureHash()
{
  return cpu_info.bCRC32 ? &TextureHashCRC32 : &TextureHashGeneric;
}
}  // namespace

u64 GetHash64(const u8* src, u32 len, u32 samples)
{
  // Resolved on first use, after cpu_info has been initialised.
  static const TextureHashFn hash = PickTextureHash();
  return hash(src, len, samples);
}

u32 StartCRC32()
{
  return crc32_z(0, nullptr, 0);
}

u32 UpdateCRC32(u32 crc, const u8* data, size_t len)
{
  return crc32_z(crc, data, len);
}

u32 ComputeCRC32(const u8* data, size_t len)
{
  return UpdateCRC32(StartCRC32(), data, len);
}

u32 ComputeCRC32(std::string_view data)
{
  return ComputeCRC32(reinterpret_cast<const u8*>(data.data()), data.size());
}
}  // namespace Common
