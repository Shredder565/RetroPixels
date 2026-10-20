// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Bulk decoder for tiled GX textures. The direct-colour formats use SSE2 (always present on
// x86-64), with SSSE3 byte shuffles where the CPU has them; the paletted formats use table
// lookups.
//
// The image is walked one tile at a time in row-major tile order, and each tile's rows are
// written top to bottom. Every tile row stores the full tile width, so callers pass
// dimensions rounded up to the tile size; for any other size the stores overlap in the same
// order as they always have.

#include "VideoCommon/TextureDecoder.h"

#include <array>
#include <cstddef>
#include <cstring>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Intrinsics.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"

#include "VideoCommon/LookUpTables.h"
#include "VideoCommon/TextureDecoder_Util.h"

using namespace TextureDecoderUtil;

namespace
{
// Every format decoder has this shape; `tlut` is only read by the paletted ones.
using FormatDecoder = void (*)(u32* dst, const u8* src, int width, int height, const u8* tlut);

// Calls tile_fn(tile_src, tile_dst) for every tile, in the order the tiles are stored.
template <int TileW, int TileH, int TileBytes, typename TileFn>
inline void ForEachTile(u32* dst, const u8* src, int width, int height, TileFn&& tile_fn)
{
  const int tiles_per_row = (width + TileW - 1) / TileW;
  for (int y = 0; y < height; y += TileH)
  {
    const u8* tile_src =
        src + static_cast<size_t>(y / TileH) * static_cast<size_t>(tiles_per_row) * TileBytes;
    u32* row_dst = dst + static_cast<ptrdiff_t>(y) * width;
    for (int x = 0; x < width; x += TileW, tile_src += TileBytes)
      tile_fn(tile_src, row_dst + x);
  }
}

// Pointer to row `row` below `p` in an image `width` texels wide.
inline u32* RowBelow(u32* p, int row, int width)
{
  return p + static_cast<ptrdiff_t>(row) * width;
}

inline __m128i Load16(const u8* p)
{
  return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
}

inline void Store4Texels(u32* p, __m128i v)
{
  _mm_storeu_si128(reinterpret_cast<__m128i*>(p), v);
}

// Replicates each nibble into a full byte: returns (high nibbles, low nibbles).
inline void SplitNibbles(__m128i v, __m128i* high, __m128i* low)
{
  const __m128i mask_lo = _mm_set1_epi8(0x0F);
  const __m128i lo = _mm_and_si128(v, mask_lo);
  const __m128i hi = _mm_andnot_si128(mask_lo, v);
  // 16-bit shifts are safe here: the bits moving across the byte boundary are masked zeros.
  *high = _mm_or_si128(hi, _mm_srli_epi16(hi, 4));
  *low = _mm_or_si128(lo, _mm_slli_epi16(lo, 4));
}

// Writes eight grey texels (one byte each, in the low 8 bytes of `bytes`) as RGBA8.
inline void StoreGreyRow8(u32* dst, __m128i bytes)
{
  const __m128i doubled = _mm_unpacklo_epi8(bytes, bytes);
  Store4Texels(dst, _mm_unpacklo_epi16(doubled, doubled));
  Store4Texels(dst + 4, _mm_unpackhi_epi16(doubled, doubled));
}

// I4: 8x8 tiles, 4 bytes (8 texels) per row, left texel in the high nibble.
void DecodeI4(u32* dst, const u8* src, int width, int height, const u8*)
{
  ForEachTile<8, 8, 32>(dst, src, width, height, [width](const u8* in, u32* out) {
    for (int half = 0; half < 2; ++half)
    {
      __m128i high, low;
      SplitNibbles(Load16(in + 16 * half), &high, &low);
      // Interleave back into texel order: rows 0-1 of this half, then rows 2-3.
      const __m128i rows01 = _mm_unpacklo_epi8(high, low);
      const __m128i rows23 = _mm_unpackhi_epi8(high, low);
      u32* rows = RowBelow(out, 4 * half, width);
      StoreGreyRow8(rows, rows01);
      StoreGreyRow8(RowBelow(rows, 1, width), _mm_srli_si128(rows01, 8));
      StoreGreyRow8(RowBelow(rows, 2, width), rows23);
      StoreGreyRow8(RowBelow(rows, 3, width), _mm_srli_si128(rows23, 8));
    }
  });
}

// I8: 8x4 tiles, one byte per texel.
void DecodeI8(u32* dst, const u8* src, int width, int height, const u8*)
{
  ForEachTile<8, 4, 32>(dst, src, width, height, [width](const u8* in, u32* out) {
    for (int pair = 0; pair < 2; ++pair)
    {
      const __m128i bytes = Load16(in + 16 * pair);
      u32* rows = RowBelow(out, 2 * pair, width);
      StoreGreyRow8(rows, bytes);
      StoreGreyRow8(RowBelow(rows, 1, width), _mm_srli_si128(bytes, 8));
    }
  });
}

// IA4: 8x4 tiles, alpha in the high nibble and intensity in the low nibble.
void DecodeIA4(u32* dst, const u8* src, int width, int height, const u8*)
{
  ForEachTile<8, 4, 32>(dst, src, width, height, [width](const u8* in, u32* out) {
    for (int pair = 0; pair < 2; ++pair)
    {
      __m128i alpha, intensity;
      SplitNibbles(Load16(in + 16 * pair), &alpha, &intensity);
      // Low 16 bits of each texel: I I. High 16 bits: I A.
      const __m128i ii_lo = _mm_unpacklo_epi8(intensity, intensity);
      const __m128i ia_lo = _mm_unpacklo_epi8(intensity, alpha);
      const __m128i ii_hi = _mm_unpackhi_epi8(intensity, intensity);
      const __m128i ia_hi = _mm_unpackhi_epi8(intensity, alpha);
      u32* first = RowBelow(out, 2 * pair, width);
      u32* second = RowBelow(first, 1, width);
      Store4Texels(first, _mm_unpacklo_epi16(ii_lo, ia_lo));
      Store4Texels(first + 4, _mm_unpackhi_epi16(ii_lo, ia_lo));
      Store4Texels(second, _mm_unpacklo_epi16(ii_hi, ia_hi));
      Store4Texels(second + 4, _mm_unpackhi_epi16(ii_hi, ia_hi));
    }
  });
}

// Runs a converter turning 8 16-bit texels (two tile rows of 4) into two vectors of RGBA8,
// over the 4x4 16-bit tiles.
template <typename Convert8>
inline void Decode16BitTiles(u32* dst, const u8* src, int width, int height, Convert8 convert)
{
  ForEachTile<4, 4, 32>(dst, src, width, height, [width, convert](const u8* in, u32* out) {
    for (int pair = 0; pair < 2; ++pair)
    {
      __m128i row_a, row_b;
      convert(Load16(in + 16 * pair), &row_a, &row_b);
      u32* rows = RowBelow(out, 2 * pair, width);
      Store4Texels(rows, row_a);
      Store4Texels(RowBelow(rows, 1, width), row_b);
    }
  });
}

// IA8: memory byte 0 is alpha, byte 1 intensity.
void DecodeIA8(u32* dst, const u8* src, int width, int height, const u8*)
{
  Decode16BitTiles(dst, src, width, height, [](__m128i raw, __m128i* first, __m128i* second) {
    const __m128i intensity = _mm_srli_epi16(raw, 8);
    const __m128i lo_half = _mm_or_si128(intensity, _mm_slli_epi16(intensity, 8));  // I I
    const __m128i hi_half = _mm_or_si128(intensity, _mm_slli_epi16(raw, 8));        // I A
    *first = _mm_unpacklo_epi16(lo_half, hi_half);
    *second = _mm_unpackhi_epi16(lo_half, hi_half);
  });
}

// IA8 with one byte shuffle per four texels.
FUNCTION_TARGET_SSSE3
void DecodeIA8_SSSE3(u32* dst, const u8* src, int width, int height, const u8*)
{
  const __m128i first_four = _mm_setr_epi8(1, 1, 1, 0, 3, 3, 3, 2, 5, 5, 5, 4, 7, 7, 7, 6);
  const __m128i last_four = _mm_add_epi8(first_four, _mm_set1_epi8(8));
  const ptrdiff_t pitch = width;
  const int tiles_per_row = (width + 3) / 4;
  for (int y = 0; y < height; y += 4)
  {
    const u8* in = src + static_cast<size_t>(y / 4) * static_cast<size_t>(tiles_per_row) * 32;
    u32* out = dst + y * pitch;
    for (int x = 0; x < width; x += 4, in += 32, out += 4)
    {
      const __m128i top = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in));
      const __m128i bottom = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_shuffle_epi8(top, first_four));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + pitch), _mm_shuffle_epi8(top, last_four));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 2 * pitch),
                       _mm_shuffle_epi8(bottom, first_four));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 3 * pitch),
                       _mm_shuffle_epi8(bottom, last_four));
    }
  }
}

inline __m128i ByteSwap16x8(__m128i v)
{
  return _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
}

// Expands n-bit channel values (one per 16-bit lane, already isolated) to 8 bits by bit
// replication.
inline __m128i Widen5(__m128i v)
{
  return _mm_or_si128(_mm_slli_epi16(v, 3), _mm_srli_epi16(v, 2));
}
inline __m128i Widen6(__m128i v)
{
  return _mm_or_si128(_mm_slli_epi16(v, 2), _mm_srli_epi16(v, 4));
}
inline __m128i Widen4(__m128i v)
{
  return _mm_or_si128(_mm_slli_epi16(v, 4), v);
}
inline __m128i Widen3(__m128i v)
{
  return _mm_or_si128(_mm_or_si128(_mm_slli_epi16(v, 5), _mm_slli_epi16(v, 2)),
                      _mm_srli_epi16(v, 1));
}

// Packs per-lane 8-bit channels (16-bit lanes) into two vectors of four RGBA8 texels.
inline void PackChannels(__m128i r, __m128i g, __m128i b, __m128i a, __m128i* first,
                         __m128i* second)
{
  const __m128i rg = _mm_or_si128(r, _mm_slli_epi16(g, 8));
  const __m128i ba = _mm_or_si128(b, _mm_slli_epi16(a, 8));
  *first = _mm_unpacklo_epi16(rg, ba);
  *second = _mm_unpackhi_epi16(rg, ba);
}

void DecodeRGB565(u32* dst, const u8* src, int width, int height, const u8*)
{
  Decode16BitTiles(dst, src, width, height, [](__m128i raw, __m128i* first, __m128i* second) {
    const __m128i v = ByteSwap16x8(raw);
    const __m128i r = Widen5(_mm_srli_epi16(v, 11));
    const __m128i g = Widen6(_mm_and_si128(_mm_srli_epi16(v, 5), _mm_set1_epi16(0x3F)));
    const __m128i b = Widen5(_mm_and_si128(v, _mm_set1_epi16(0x1F)));
    PackChannels(r, g, b, _mm_set1_epi16(0xFF), first, second);
  });
}

inline __m128i Select(__m128i mask, __m128i if_set, __m128i if_clear)
{
  return _mm_or_si128(_mm_and_si128(mask, if_set), _mm_andnot_si128(mask, if_clear));
}

void DecodeRGB5A3(u32* dst, const u8* src, int width, int height, const u8*)
{
  Decode16BitTiles(dst, src, width, height, [](__m128i raw, __m128i* first, __m128i* second) {
    const __m128i v = ByteSwap16x8(raw);
    const __m128i five = _mm_set1_epi16(0x1F);
    const __m128i four = _mm_set1_epi16(0x0F);
    // Top bit set: opaque RGB555. Clear: alpha in 3 bits, then RGB444.
    const __m128i opaque = _mm_srai_epi16(v, 15);
    const __m128i r = Select(opaque, Widen5(_mm_and_si128(_mm_srli_epi16(v, 10), five)),
                             Widen4(_mm_and_si128(_mm_srli_epi16(v, 8), four)));
    const __m128i g = Select(opaque, Widen5(_mm_and_si128(_mm_srli_epi16(v, 5), five)),
                             Widen4(_mm_and_si128(_mm_srli_epi16(v, 4), four)));
    const __m128i b =
        Select(opaque, Widen5(_mm_and_si128(v, five)), Widen4(_mm_and_si128(v, four)));
    const __m128i a =
        Select(opaque, _mm_set1_epi16(0xFF),
               Widen3(_mm_and_si128(_mm_srli_epi16(v, 12), _mm_set1_epi16(0x07))));
    PackChannels(r, g, b, a, first, second);
  });
}

// RGBA8: each 4x4 tile is 32 bytes of (A, R) pairs, then 32 bytes of (G, B) pairs.
void DecodeRGBA8(u32* dst, const u8* src, int width, int height, const u8*)
{
  ForEachTile<4, 4, 64>(dst, src, width, height, [width](const u8* in, u32* out) {
    for (int pair = 0; pair < 2; ++pair)
    {
      const __m128i ar = Load16(in + 16 * pair);
      const __m128i gb = Load16(in + 32 + 16 * pair);
      const __m128i rg = _mm_or_si128(_mm_srli_epi16(ar, 8), _mm_slli_epi16(gb, 8));
      const __m128i ba = _mm_or_si128(_mm_srli_epi16(gb, 8), _mm_slli_epi16(ar, 8));
      u32* rows = RowBelow(out, 2 * pair, width);
      Store4Texels(rows, _mm_unpacklo_epi16(rg, ba));
      Store4Texels(RowBelow(rows, 1, width), _mm_unpackhi_epi16(rg, ba));
    }
  });
}

// RGBA8 with SSSE3: interleaving the AR and GB bytes gives A G R B per texel, which one
// shuffle puts in R G B A order.
FUNCTION_TARGET_SSSE3
void DecodeRGBA8_SSSE3(u32* dst, const u8* src, int width, int height, const u8*)
{
  const __m128i to_rgba = _mm_setr_epi8(2, 1, 3, 0, 6, 5, 7, 4, 10, 9, 11, 8, 14, 13, 15, 12);
  const ptrdiff_t pitch = width;
  const int tiles_per_row = (width + 3) / 4;
  for (int y = 0; y < height; y += 4)
  {
    const u8* in = src + static_cast<size_t>(y / 4) * static_cast<size_t>(tiles_per_row) * 64;
    u32* out = dst + y * pitch;
    for (int x = 0; x < width; x += 4, in += 64, out += 4)
    {
      const __m128i ar_top = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in));
      const __m128i ar_bottom = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 16));
      const __m128i gb_top = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 32));
      const __m128i gb_bottom = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + 48));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out),
                       _mm_shuffle_epi8(_mm_unpacklo_epi8(ar_top, gb_top), to_rgba));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + pitch),
                       _mm_shuffle_epi8(_mm_unpackhi_epi8(ar_top, gb_top), to_rgba));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 2 * pitch),
                       _mm_shuffle_epi8(_mm_unpacklo_epi8(ar_bottom, gb_bottom), to_rgba));
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 3 * pitch),
                       _mm_shuffle_epi8(_mm_unpackhi_epi8(ar_bottom, gb_bottom), to_rgba));
    }
  }
}

// Palette entries, with the conversion picked at compile time.
template <TLUTFormat Format>
inline u32 PaletteEntry(const u8* tlut, u32 index)
{
  u16 raw;
  std::memcpy(&raw, tlut + 2 * index, sizeof(raw));
  if constexpr (Format == TLUTFormat::IA8)
    return IA8FromRaw(raw);
  else if constexpr (Format == TLUTFormat::RGB565)
    return RGB565FromValue(Common::swap16(raw));
  else
    return RGB5A3FromValue(Common::swap16(raw));
}

// C4: like I4, each nibble indexes a 16-entry palette, converted once up front.
template <TLUTFormat Format>
void DecodeC4(u32* dst, const u8* src, int width, int height, const u8* tlut)
{
  std::array<u32, 16> palette;
  for (u32 i = 0; i < palette.size(); ++i)
    palette[i] = PaletteEntry<Format>(tlut, i);
  ForEachTile<8, 8, 32>(dst, src, width, height, [width, &palette](const u8* in, u32* out) {
    for (int row = 0; row < 8; ++row, in += 4, out += width)
    {
      for (int i = 0; i < 4; ++i)
      {
        out[2 * i] = palette[in[i] >> 4];
        out[2 * i + 1] = palette[in[i] & 0xF];
      }
    }
  });
}

// C8: 8x4 tiles of 8-bit indices. Converting all 256 entries first only pays off for
// textures with at least that many texels.
template <TLUTFormat Format>
void DecodeC8(u32* dst, const u8* src, int width, int height, const u8* tlut)
{
  if (width * height >= 256)
  {
    std::array<u32, 256> palette;
    for (u32 i = 0; i < palette.size(); ++i)
      palette[i] = PaletteEntry<Format>(tlut, i);
    ForEachTile<8, 4, 32>(dst, src, width, height, [width, &palette](const u8* in, u32* out) {
      for (int row = 0; row < 4; ++row, in += 8, out += width)
      {
        for (int i = 0; i < 8; ++i)
          out[i] = palette[in[i]];
      }
    });
    return;
  }

  ForEachTile<8, 4, 32>(dst, src, width, height, [width, tlut](const u8* in, u32* out) {
    for (int row = 0; row < 4; ++row, in += 8, out += width)
    {
      for (int i = 0; i < 8; ++i)
        out[i] = PaletteEntry<Format>(tlut, in[i]);
    }
  });
}

// C14X2: 4x4 tiles of big-endian 16-bit values whose low 14 bits index the palette.
template <TLUTFormat Format>
void DecodeC14X2(u32* dst, const u8* src, int width, int height, const u8* tlut)
{
  ForEachTile<4, 4, 32>(dst, src, width, height, [width, tlut](const u8* in, u32* out) {
    for (int row = 0; row < 4; ++row, in += 8, out += width)
    {
      for (int i = 0; i < 4; ++i)
        out[i] = PaletteEntry<Format>(tlut, Common::swap16(in + 2 * i) & 0x3FFFu);
    }
  });
}

// CMPR: 8x8 tiles made of four 4x4 sub-blocks (top-left, top-right, bottom-left,
// bottom-right), 8 bytes each. Each pair of side-by-side sub-blocks is 16 bytes and fills
// four rows of 8 texels.
//
// Builds the 4-colour palettes of both sub-blocks of a pair at once. In the byte-swapped
// 16-bit lanes, lanes 0/1 are the left block's endpoints and lanes 4/5 the right block's.
inline void CMPRPalettePair(__m128i raw, __m128i* left, __m128i* right)
{
  const __m128i v = ByteSwap16x8(raw);
  // Each lane next to its partner endpoint (lanes 0<->1, 2<->3, ...).
  const auto partner = [](__m128i x) {
    return _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, 0xB1), 0xB1);
  };
  const __m128i sign = _mm_set1_epi16(static_cast<short>(0x8000));
  const __m128i first_greater =
      _mm_cmpgt_epi16(_mm_xor_si128(v, sign), _mm_xor_si128(partner(v), sign));
  // Lane 0 (lane 4) says whether the left (right) block uses four opaque colours.
  const __m128i four_colors =
      _mm_shufflehi_epi16(_mm_shufflelo_epi16(first_greater, 0x00), 0x00);

  // Interpolated colours: in lane 0 the one nearer the second endpoint (palette entry 3),
  // in lane 1 the one nearer the first (entry 2); otherwise the average.
  const auto mix = [four_colors, partner](__m128i ch) {
    const __m128i other = partner(ch);
    const __m128i blend =
        _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(ch, _mm_set1_epi16(3)),
                                     _mm_mullo_epi16(other, _mm_set1_epi16(5))),
                       3);
    const __m128i average = _mm_srli_epi16(_mm_add_epi16(ch, other), 1);
    return Select(four_colors, blend, average);
  };

  const __m128i r = Widen5(_mm_srli_epi16(v, 11));
  const __m128i g = Widen6(_mm_and_si128(_mm_srli_epi16(v, 5), _mm_set1_epi16(0x3F)));
  const __m128i b = Widen5(_mm_and_si128(v, _mm_set1_epi16(0x1F)));
  const __m128i opaque = _mm_set1_epi16(0xFF);

  // Entry 2 is always opaque; entry 3 only in four-colour mode.
  const __m128i entry2_lanes = _mm_setr_epi16(0, -1, 0, 0, 0, -1, 0, 0);
  const __m128i mixed_alpha = _mm_and_si128(opaque, _mm_or_si128(four_colors, entry2_lanes));

  const __m128i rg_end = _mm_or_si128(r, _mm_slli_epi16(g, 8));
  const __m128i ba_end = _mm_or_si128(b, _mm_slli_epi16(opaque, 8));
  const __m128i rg_mix = _mm_or_si128(mix(r), _mm_slli_epi16(mix(g), 8));
  const __m128i ba_mix = _mm_or_si128(mix(b), _mm_slli_epi16(mixed_alpha, 8));

  // [end0, end1, x, x] and [entry3, entry2, x, x] per block, as RGBA8.
  const __m128i ends_left = _mm_unpacklo_epi16(rg_end, ba_end);
  const __m128i ends_right = _mm_unpackhi_epi16(rg_end, ba_end);
  const __m128i mixes_left = _mm_unpacklo_epi16(rg_mix, ba_mix);
  const __m128i mixes_right = _mm_unpackhi_epi16(rg_mix, ba_mix);
  *left = _mm_unpacklo_epi64(ends_left, _mm_shuffle_epi32(mixes_left, _MM_SHUFFLE(3, 2, 0, 1)));
  *right =
      _mm_unpacklo_epi64(ends_right, _mm_shuffle_epi32(mixes_right, _MM_SHUFFLE(3, 2, 0, 1)));
}

void DecodeCMPR(u32* dst, const u8* src, int width, int height, const u8*)
{
  ForEachTile<8, 8, 32>(dst, src, width, height, [width](const u8* in, u32* out) {
    for (int half = 0; half < 2; ++half, in += 16)
    {
      alignas(16) u32 left[4];
      alignas(16) u32 right[4];
      __m128i left_v, right_v;
      CMPRPalettePair(Load16(in), &left_v, &right_v);
      _mm_store_si128(reinterpret_cast<__m128i*>(left), left_v);
      _mm_store_si128(reinterpret_cast<__m128i*>(right), right_v);

      u32* rows = RowBelow(out, 4 * half, width);
      for (int row = 0; row < 4; ++row, rows += width)
      {
        const u32 left_sel = in[4 + row];
        const u32 right_sel = in[12 + row];
        for (int i = 0; i < 4; ++i)
          rows[i] = left[(left_sel >> (6 - 2 * i)) & 3];
        for (int i = 0; i < 4; ++i)
          rows[4 + i] = right[(right_sel >> (6 - 2 * i)) & 3];
      }
    }
  });
}

// For the SSSE3 CMPR path: the byte shuffle that turns a 4-colour palette register into the
// four texels selected by one selector byte (2 bits per texel, leftmost in the top bits).
struct alignas(16) SelectorShuffle
{
  u8 bytes[16];
};

constexpr std::array<SelectorShuffle, 256> MakeSelectorShuffles()
{
  std::array<SelectorShuffle, 256> table{};
  for (u32 sel = 0; sel < 256; ++sel)
  {
    for (u32 texel = 0; texel < 4; ++texel)
    {
      const u32 entry = (sel >> (6 - 2 * texel)) & 3;
      for (u32 byte = 0; byte < 4; ++byte)
        table[sel].bytes[4 * texel + byte] = static_cast<u8>(4 * entry + byte);
    }
  }
  return table;
}

alignas(16) constexpr std::array<SelectorShuffle, 256> s_selector_shuffles = MakeSelectorShuffles();

FUNCTION_TARGET_SSSE3
void DecodeCMPR_SSSE3(u32* dst, const u8* src, int width, int height, const u8*)
{
  const ptrdiff_t pitch = width;
  const int tiles_per_row = (width + 7) / 8;
  for (int y = 0; y < height; y += 8)
  {
    const u8* in = src + static_cast<size_t>(y / 8) * static_cast<size_t>(tiles_per_row) * 32;
    u32* tile_out = dst + y * pitch;
    for (int x = 0; x < width; x += 8, tile_out += 8)
    {
      for (int half = 0; half < 2; ++half, in += 16)
      {
        __m128i left, right;
        CMPRPalettePair(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in)), &left, &right);
        u32* out = tile_out + 4 * half * pitch;
        for (int row = 0; row < 4; ++row, out += pitch)
        {
          const __m128i left_pick = _mm_load_si128(
              reinterpret_cast<const __m128i*>(s_selector_shuffles[in[4 + row]].bytes));
          const __m128i right_pick = _mm_load_si128(
              reinterpret_cast<const __m128i*>(s_selector_shuffles[in[12 + row]].bytes));
          _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_shuffle_epi8(left, left_pick));
          _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 4),
                           _mm_shuffle_epi8(right, right_pick));
        }
      }
    }
  }
}

void DecodeNothing(u32*, const u8*, int, int, const u8*)
{
}

// Decoders for one paletted format, indexed by TLUT format; an unknown TLUT format decodes
// nothing.
template <template <TLUTFormat> class Decoder>
constexpr std::array<FormatDecoder, 4> PalettedDecoders()
{
  return {&Decoder<TLUTFormat::IA8>::Run, &Decoder<TLUTFormat::RGB565>::Run,
          &Decoder<TLUTFormat::RGB5A3>::Run, &DecodeNothing};
}

template <TLUTFormat F>
struct C4Decoder
{
  static void Run(u32* d, const u8* s, int w, int h, const u8* t) { DecodeC4<F>(d, s, w, h, t); }
};
template <TLUTFormat F>
struct C8Decoder
{
  static void Run(u32* d, const u8* s, int w, int h, const u8* t) { DecodeC8<F>(d, s, w, h, t); }
};
template <TLUTFormat F>
struct C14X2Decoder
{
  static void Run(u32* d, const u8* s, int w, int h, const u8* t)
  {
    DecodeC14X2<F>(d, s, w, h, t);
  }
};

constexpr std::array<FormatDecoder, 4> s_c4_decoders = PalettedDecoders<C4Decoder>();
constexpr std::array<FormatDecoder, 4> s_c8_decoders = PalettedDecoders<C8Decoder>();
constexpr std::array<FormatDecoder, 4> s_c14x2_decoders = PalettedDecoders<C14X2Decoder>();

FormatDecoder PickPaletted(const std::array<FormatDecoder, 4>& decoders, TLUTFormat tlutfmt)
{
  const u32 index = static_cast<u32>(tlutfmt);
  return decoders[index < 3 ? index : 3];
}
}  // namespace

void _TexDecoder_DecodeImpl(u32* dst, const u8* src, int width, int height, TextureFormat texformat,
                            const u8* tlut, TLUTFormat tlutfmt)
{
  const bool ssse3 = cpu_info.bSSSE3;
  FormatDecoder decoder = nullptr;
  switch (texformat)
  {
  case TextureFormat::I4:
    decoder = &DecodeI4;
    break;
  case TextureFormat::I8:
    decoder = &DecodeI8;
    break;
  case TextureFormat::IA4:
    decoder = &DecodeIA4;
    break;
  case TextureFormat::IA8:
    decoder = ssse3 ? &DecodeIA8_SSSE3 : &DecodeIA8;
    break;
  case TextureFormat::RGB565:
    decoder = &DecodeRGB565;
    break;
  case TextureFormat::RGB5A3:
    decoder = &DecodeRGB5A3;
    break;
  case TextureFormat::RGBA8:
    decoder = ssse3 ? &DecodeRGBA8_SSSE3 : &DecodeRGBA8;
    break;
  case TextureFormat::C4:
    decoder = PickPaletted(s_c4_decoders, tlutfmt);
    break;
  case TextureFormat::C8:
    decoder = PickPaletted(s_c8_decoders, tlutfmt);
    break;
  case TextureFormat::C14X2:
    decoder = PickPaletted(s_c14x2_decoders, tlutfmt);
    break;
  case TextureFormat::CMPR:
    decoder = ssse3 ? &DecodeCMPR_SSSE3 : &DecodeCMPR;
    break;
  case TextureFormat::XFB:
    TexDecoder_DecodeXFB(reinterpret_cast<u8*>(dst), src, static_cast<u32>(width),
                         static_cast<u32>(height), static_cast<u32>(width) * 2);
    return;
  default:
    PanicAlertFmt("Invalid Texture Format {}! (_TexDecoder_DecodeImpl)", texformat);
    return;
  }
  decoder(dst, src, width, height, tlut);
}
