// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Common/SpanUtils.h"
#include "Common/Swap.h"

#include "VideoCommon/LookUpTables.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/TextureDecoder_Util.h"
#include "VideoCommon/sfont.inc"

using namespace TextureDecoderUtil;

// TMEM contents (saved in savestates by the BP/pixel engine code).
alignas(16) std::array<u8, TMEM_SIZE> s_tex_mem;

namespace
{
bool s_overlay_enabled = false;
bool s_overlay_centered = false;

// Texel size and tile shape of every format the decoders understand.
struct FormatShape
{
  bool known;
  int nibbles_per_texel;
  int tile_width;
  int tile_height;
};

constexpr FormatShape ShapeOf(TextureFormat format)
{
  switch (format)
  {
  case TextureFormat::I4:
  case TextureFormat::C4:
    return {true, 1, 8, 8};
  case TextureFormat::I8:
  case TextureFormat::IA4:
  case TextureFormat::C8:
    return {true, 2, 8, 4};
  case TextureFormat::IA8:
  case TextureFormat::RGB565:
  case TextureFormat::RGB5A3:
  case TextureFormat::C14X2:
    return {true, 4, 4, 4};
  case TextureFormat::RGBA8:
    return {true, 8, 4, 4};
  case TextureFormat::CMPR:
    return {true, 1, 8, 8};
  case TextureFormat::XFB:
    return {true, 4, 16, 1};
  default:
    return {false, 0, 0, 0};
  }
}

// Tile shape of every copy encoder (RGBA8 copies write two 32-byte lines per 4x4 tile, but
// the tile is still 4x4 texels).
constexpr FormatShape ShapeOf(EFBCopyFormat format)
{
  switch (format)
  {
  case EFBCopyFormat::R4:
    return {true, 1, 8, 8};
  case EFBCopyFormat::RA4:
  case EFBCopyFormat::A8:
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
  case EFBCopyFormat::G8:
  case EFBCopyFormat::B8:
    return {true, 2, 8, 4};
  case EFBCopyFormat::RA8:
  case EFBCopyFormat::RGB565:
  case EFBCopyFormat::RGB5A3:
  case EFBCopyFormat::RG8:
  case EFBCopyFormat::GB8:
    return {true, 4, 4, 4};
  case EFBCopyFormat::RGBA8:
    return {true, 8, 4, 4};
  case EFBCopyFormat::XFB:
    return {true, 4, 16, 1};
  default:
    return {false, 0, 0, 0};
  }
}

u32 PaletteLookup(std::span<const u8> tlut, u32 index, TLUTFormat tlutfmt)
{
  const u16 raw = Common::SafeSpanRead<u16>(tlut, sizeof(u16) * index);
  switch (tlutfmt)
  {
  case TLUTFormat::IA8:
    return IA8FromRaw(raw);
  case TLUTFormat::RGB565:
    return RGB565FromValue(Common::swap16(raw));
  case TLUTFormat::RGB5A3:
    return RGB5A3FromValue(Common::swap16(raw));
  default:
    return 0;
  }
}

void StoreTexel(u8* dst, u32 rgba)
{
  std::memcpy(dst, &rgba, sizeof(rgba));
}

// Position of texel (s, t) in a tiled image. The tile counts per row follow what the software
// sampler passes in `image_width` (the width minus one): one more tile than width >> shift.
// The 16-bit truncations match the GX coordinate width.
struct TilePosition
{
  u32 tile;    // tile number, row-major
  u32 within;  // texel number inside the tile, row-major
};

TilePosition LocateTexel(int s, int t, int width_shift, int height_shift, int image_width)
{
  const u16 tile_x = static_cast<u16>(s >> width_shift);
  const u16 tile_y = static_cast<u16>(t >> height_shift);
  const u16 tiles_per_row = static_cast<u16>((image_width >> width_shift) + 1);
  const u32 in_x = static_cast<u32>(s) & ((1u << width_shift) - 1);
  const u32 in_y = static_cast<u32>(t) & ((1u << height_shift) - 1);
  return {static_cast<u32>(tile_y) * tiles_per_row + tile_x, (in_y << width_shift) + in_x};
}

// Offset (in texels of the format) of texel (s, t) for formats whose tile is one contiguous
// run of texels.
u32 TiledTexelIndex(int s, int t, int width_shift, int height_shift, int image_width)
{
  const TilePosition pos = LocateTexel(s, t, width_shift, height_shift, image_width);
  return (pos.tile << (width_shift + height_shift)) + pos.within;
}

u8 ClampToByte(float value)
{
  return static_cast<u8>(std::clamp(static_cast<int>(value), 0, 255));
}

// Inverse BT.601 (studio swing) YCbCr to RGB, as used for XFB textures. y, u and v have
// their offsets (16, 128, 128) already removed.
u32 YUVToRGBA(int y_int, int u_int, int v_int)
{
  // The inputs are at most 9 bits, so these conversions are exact.
  const float y = static_cast<float>(y_int);
  const float u = static_cast<float>(u_int);
  const float v = static_cast<float>(v_int);
  const u8 r = ClampToByte(1.164f * y + 1.596f * v);
  const u8 g = ClampToByte(1.164f * y - 0.392f * u - 0.813f * v);
  const u8 b = ClampToByte(1.164f * y + 2.017f * u);
  return 0xFF000000u | static_cast<u32>(b) << 16 | static_cast<u32>(g) << 8 | r;
}

u32 DecodeCMPRTexel(std::span<const u8> src, int s, int t, int image_width)
{
  // Each 8x8 tile holds four 4x4 sub-blocks of 8 bytes, in row-major order.
  const u16 sub_x = static_cast<u16>(s >> 2);
  const u16 sub_y = static_cast<u16>(t >> 2);
  const u16 tiles_per_row = static_cast<u16>((image_width >> 3) + 1);
  const u32 tile = static_cast<u32>(sub_y >> 1) * tiles_per_row + (sub_x >> 1);
  const u32 sub_block = (tile << 2) + ((sub_y & 1u) << 1) + (sub_x & 1u);

  const DXTBlock block = Common::SafeSpanRead<DXTBlock>(src, sub_block << 3);
  const std::array<u32, 4> colors = CMPRPalette(block.color1, block.color2);
  const u32 selector_shift = 6 - ((static_cast<u32>(s) & 3) << 1);
  return colors[(block.lines[t & 3] >> selector_shift) & 3];
}

void DrawFormatName(u8* dst, int width, int height, TextureFormat texformat)
{
  int x_pos = 0;
  int y_pos = 0;
  if (s_overlay_centered)
  {
    x_pos = (width - std::min(width, 40)) >> 1;
    y_pos = (height - std::min(height, 10)) >> 1;
  }

  u32* const texels = reinterpret_cast<u32*>(dst);
  for (const char ch : fmt::to_string(texformat))
  {
    // Glyphs are 10 rows of 9 cells; unused cells on the right hold 0x78.
    const unsigned char* glyph = sfont_raw[sfont_map[static_cast<u8>(ch)]];
    int glyph_width = 0;
    while (glyph_width < 9 && glyph[glyph_width] != 0x78)
      ++glyph_width;

    for (int row = 0; row < 10; ++row)
    {
      const unsigned char* cells = glyph + row * 9;
      u32* line = texels + (row + y_pos) * width + x_pos;
      for (int col = 0; col < glyph_width; ++col)
        line[col] = cells[col] ? 0xFFFFFFFFu : 0xFF000000u;
    }
    x_pos += glyph_width;
  }
}
}  // namespace

int TexDecoder_GetTexelSizeInNibbles(TextureFormat format)
{
  const FormatShape shape = ShapeOf(format);
  if (shape.known)
    return shape.nibbles_per_texel;
  PanicAlertFmt("Invalid Texture Format {}! (GetTexelSizeInNibbles)", format);
  return 1;
}

int TexDecoder_GetTextureSizeInBytes(int width, int height, TextureFormat format)
{
  return (width * height * TexDecoder_GetTexelSizeInNibbles(format)) / 2;
}

int TexDecoder_GetBlockWidthInTexels(TextureFormat format)
{
  const FormatShape shape = ShapeOf(format);
  if (shape.known)
    return shape.tile_width;
  PanicAlertFmt("Invalid Texture Format {}! (GetBlockWidthInTexels)", format);
  return 8;
}

int TexDecoder_GetBlockHeightInTexels(TextureFormat format)
{
  const FormatShape shape = ShapeOf(format);
  if (shape.known)
    return shape.tile_height;
  PanicAlertFmt("Invalid Texture Format {}! (GetBlockHeightInTexels)", format);
  return 4;
}

int TexDecoder_GetEFBCopyBlockWidthInTexels(EFBCopyFormat format)
{
  const FormatShape shape = ShapeOf(format);
  if (shape.known)
    return shape.tile_width;
  PanicAlertFmt("Invalid EFB Copy Format {}! (GetEFBCopyBlockWidthInTexels)", format);
  return 8;
}

int TexDecoder_GetEFBCopyBlockHeightInTexels(EFBCopyFormat format)
{
  const FormatShape shape = ShapeOf(format);
  if (shape.known)
    return shape.tile_height;
  PanicAlertFmt("Invalid EFB Copy Format {}! (GetEFBCopyBlockHeightInTexels)", format);
  return 4;
}

int TexDecoder_GetPaletteSize(TextureFormat format)
{
  // Bytes of TLUT a paletted format can address: 2 bytes per entry.
  switch (format)
  {
  case TextureFormat::C4:
    return 2 << 4;
  case TextureFormat::C8:
    return 2 << 8;
  case TextureFormat::C14X2:
    return 2 << 14;
  default:
    return 0;
  }
}

TextureFormat TexDecoder_GetEFBCopyBaseFormat(EFBCopyFormat format)
{
  // The texture format whose memory layout an EFB copy produces, i.e. the format a game reads
  // the copy back with (paletted formats share these layouts). Indexed by the copy format's
  // GX value 0x0-0xC; XFB copies are handled separately.
  static constexpr std::array<TextureFormat, 13> layout_of = {
      TextureFormat::I4,      // R4
      TextureFormat::I8,      // R8_0x1
      TextureFormat::IA4,     // RA4
      TextureFormat::IA8,     // RA8
      TextureFormat::RGB565,  // RGB565
      TextureFormat::RGB5A3,  // RGB5A3
      TextureFormat::RGBA8,   // RGBA8
      TextureFormat::I8,      // A8
      TextureFormat::I8,      // R8
      TextureFormat::I8,      // G8
      TextureFormat::I8,      // B8
      TextureFormat::IA8,     // RG8
      TextureFormat::IA8,     // GB8
  };
  const auto value = static_cast<u32>(format);
  if (value < layout_of.size())
    return layout_of[value];
  if (format == EFBCopyFormat::XFB)
    return TextureFormat::XFB;
  PanicAlertFmt("Invalid EFB Copy Format {}! (GetEFBCopyBaseFormat)", format);
  return static_cast<TextureFormat>(format);
}

void TexDecoder_SetTexFmtOverlayOptions(bool enable, bool center)
{
  s_overlay_enabled = enable;
  s_overlay_centered = center;
}

void TexDecoder_Decode(u8* dst, const u8* src, int width, int height, TextureFormat texformat,
                       const u8* tlut, TLUTFormat tlutfmt)
{
  _TexDecoder_DecodeImpl(reinterpret_cast<u32*>(dst), src, width, height, texformat, tlut,
                         tlutfmt);
  if (s_overlay_enabled)
    DrawFormatName(dst, width, height, texformat);
}

void TexDecoder_DecodeTexel(u8* dst, std::span<const u8> src, int s, int t, int imageWidth,
                            TextureFormat texformat, std::span<const u8> tlut, TLUTFormat tlutfmt)
{
  switch (texformat)
  {
  case TextureFormat::I4:
  case TextureFormat::C4:
  {
    // Two texels per byte, the left one in the high nibble.
    const u32 index = TiledTexelIndex(s, t, 3, 3, imageWidth);
    const u8 packed = Common::SafeSpanRead<u8>(src, index >> 1);
    const u8 nibble = (index & 1) ? (packed & 0xF) : (packed >> 4);
    if (texformat == TextureFormat::C4)
    {
      StoreTexel(dst, PaletteLookup(tlut, nibble, tlutfmt));
    }
    else
    {
      const u8 intensity = Convert4To8(nibble);
      std::memset(dst, intensity, 4);
    }
    break;
  }
  case TextureFormat::I8:
  case TextureFormat::C8:
  case TextureFormat::IA4:
  {
    const u8 value = Common::SafeSpanRead<u8>(src, TiledTexelIndex(s, t, 3, 2, imageWidth));
    if (texformat == TextureFormat::I8)
    {
      std::memset(dst, value, 4);
    }
    else if (texformat == TextureFormat::C8)
    {
      StoreTexel(dst, PaletteLookup(tlut, value, tlutfmt));
    }
    else
    {
      // IA4: alpha in the high nibble, intensity in the low one.
      const u8 intensity = Convert4To8(value & 0xF);
      dst[0] = intensity;
      dst[1] = intensity;
      dst[2] = intensity;
      dst[3] = Convert4To8(value >> 4);
    }
    break;
  }
  case TextureFormat::IA8:
  case TextureFormat::C14X2:
  case TextureFormat::RGB565:
  case TextureFormat::RGB5A3:
  {
    const u32 byte_offset = TiledTexelIndex(s, t, 2, 2, imageWidth) << 1;
    const u16 raw = Common::SafeSpanRead<u16>(src, byte_offset);
    u32 rgba;
    if (texformat == TextureFormat::IA8)
      rgba = IA8FromRaw(raw);
    else if (texformat == TextureFormat::C14X2)
      rgba = PaletteLookup(tlut, Common::swap16(raw) & 0x3FFFu, tlutfmt);
    else if (texformat == TextureFormat::RGB565)
      rgba = RGB565FromValue(Common::swap16(raw));
    else
      rgba = RGB5A3FromValue(Common::swap16(raw));
    StoreTexel(dst, rgba);
    break;
  }
  case TextureFormat::RGBA8:
  {
    // A 4x4 tile is 32 bytes of AR pairs followed by 32 bytes of GB pairs.
    const TilePosition pos = LocateTexel(s, t, 2, 2, imageWidth);
    const u32 ar_offset = ((pos.tile << 5) + pos.within) << 1;
    dst[3] = Common::SafeSpanRead<u8>(src, ar_offset);
    dst[0] = Common::SafeSpanRead<u8>(src, ar_offset + 1);
    dst[1] = Common::SafeSpanRead<u8>(src, ar_offset + 32);
    dst[2] = Common::SafeSpanRead<u8>(src, ar_offset + 33);
    break;
  }
  case TextureFormat::CMPR:
    StoreTexel(dst, DecodeCMPRTexel(src, s, t, imageWidth));
    break;
  case TextureFormat::XFB:
  {
    // Linear YUYV: each 4-byte group holds two texels sharing U and V. Only the low byte of
    // the result (red) is stored, at a byte index of t * width + s.
    const size_t pair = static_cast<size_t>((t * imageWidth + (s & ~1)) * 2);
    const int y = int(src[(s & 1) ? pair + 2 : pair]) - 16;
    const int u = int(src[pair + 1]) - 128;
    const int v = int(src[pair + 3]) - 128;
    dst[t * imageWidth + s] = static_cast<u8>(YUVToRGBA(y, u, v));
    break;
  }
  default:
    break;
  }
}

void TexDecoder_DecodeTexelRGBA8FromTmem(u8* dst, std::span<const u8> src_ar,
                                         std::span<const u8> src_gb, int s, int t, int imageWidth)
{
  // Both banks use the 16-bit 4x4 tiling; one holds AR pairs, the other GB pairs.
  const u32 offset = TiledTexelIndex(s, t, 2, 2, imageWidth) << 1;
  dst[3] = Common::SafeSpanRead<u8>(src_ar, offset);
  dst[0] = Common::SafeSpanRead<u8>(src_ar, offset + 1);
  dst[1] = Common::SafeSpanRead<u8>(src_gb, offset);
  dst[2] = Common::SafeSpanRead<u8>(src_gb, offset + 1);
}

void TexDecoder_DecodeTexelRGBA8FromTmem(u8* dst, const u8* src_ar, const u8* src_gb, int s, int t,
                                         int imageWidth)
{
  const u32 offset = TiledTexelIndex(s, t, 2, 2, imageWidth) << 1;
  dst[3] = src_ar[offset];
  dst[0] = src_ar[offset + 1];
  dst[1] = src_gb[offset];
  dst[2] = src_gb[offset + 1];
}

void TexDecoder_DecodeRGBA8FromTmem(u8* dst, const u8* src_ar, const u8* src_gb, int width,
                                    int height)
{
  if (width <= 0 || height <= 0)
    return;

  // Same layout as the per-texel decoder with an image width of width - 1, walked tile by
  // tile instead of recomputing every offset.
  const u32 tiles_per_row = static_cast<u32>(((width - 1) >> 2) + 1);
  const u32 row_pitch = static_cast<u32>(width) * 4;
  for (int y = 0; y < height; ++y)
  {
    const u32 tile_row_base = (static_cast<u32>(y) >> 2) * tiles_per_row * 32 + (y & 3) * 8;
    u8* out = dst + static_cast<u32>(y) * row_pitch;
    for (int x = 0; x < width; ++x, out += 4)
    {
      const u32 offset = tile_row_base + (static_cast<u32>(x) >> 2) * 32 + (x & 3) * 2;
      out[0] = src_ar[offset + 1];
      out[1] = src_gb[offset];
      out[2] = src_gb[offset + 1];
      out[3] = src_ar[offset];
    }
  }
}

void TexDecoder_DecodeXFB(u8* dst, const u8* src, u32 width, u32 height, u32 stride)
{
  // Each row is width / 2 (rounded up) YUYV groups; the output is packed without padding.
  const u32 groups_per_row = (width + 1) / 2;
  u8* out = dst;
  for (u32 row = 0; row < height; ++row)
  {
    const u8* in = src + static_cast<size_t>(row) * stride;
    for (u32 group = 0; group < groups_per_row; ++group, in += 4)
    {
      const int y0 = int(in[0]) - 16;
      const int u = int(in[1]) - 128;
      const int y1 = int(in[2]) - 16;
      const int v = int(in[3]) - 128;
      StoreTexel(out, YUVToRGBA(y0, u, v));
      StoreTexel(out + 4, YUVToRGBA(y1, u, v));
      out += 8;
    }
  }
}
