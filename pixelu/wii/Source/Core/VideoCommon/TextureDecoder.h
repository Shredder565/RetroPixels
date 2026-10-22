// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <span>
#include <tuple>

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "Common/SpanUtils.h"

// Texture memory (TMEM) of the GX: 1 MiB, filled in 32-byte lines by texture preloads and
// TLUT loads.
enum
{
  TMEM_SIZE = 1024 * 1024,
  TMEM_LINE_SIZE = 32,
};
alignas(16) extern std::array<u8, TMEM_SIZE> s_tex_mem;

// Texel formats as written to the TX_SETIMAGE0 format field.
enum class TextureFormat
{
  I4 = 0x0,
  I8 = 0x1,
  IA4 = 0x2,
  IA8 = 0x3,
  RGB565 = 0x4,
  RGB5A3 = 0x5,
  RGBA8 = 0x6,
  C4 = 0x8,
  C8 = 0x9,
  C14X2 = 0xA,
  CMPR = 0xE,

  // Not a GX value. The texture cache uses it for YUYV external framebuffer copies, which go
  // through the same paths as textures.
  XFB = 0xF,
};
template <>
struct fmt::formatter<TextureFormat> : EnumFormatter<TextureFormat::CMPR>
{
  static constexpr array_type names = {"I4",     "I8",    "IA4",   "IA8",   "RGB565",
                                       "RGB5A3", "RGBA8", nullptr, "C4",    "C8",
                                       "C14X2",  nullptr, nullptr, nullptr, "CMPR"};
  constexpr formatter() : EnumFormatter(names) {}
};

static inline bool IsColorIndexed(TextureFormat format)
{
  switch (format)
  {
  case TextureFormat::C4:
  case TextureFormat::C8:
  case TextureFormat::C14X2:
    return true;
  default:
    return false;
  }
}

static inline bool IsValidTextureFormat(TextureFormat format)
{
  return format <= TextureFormat::RGBA8 || IsColorIndexed(format) || format == TextureFormat::CMPR;
}

// Pixel engine copy formats (the PE copy control "target format" field together with its
// high bit). The copy unit picks colour or depth, optionally box-filters and converts to YUV,
// and then packs RGBA8 texels into tiles with one of these encoders. The encoder itself does
// not know whether it is packing colour, intensity or depth.
enum class EFBCopyFormat
{
  R4 = 0x0,      // R4, I4, Z4
  R8_0x1 = 0x1,  // R8, I8, Z8H (the low value of the 8-bit red encoders)
  RA4 = 0x2,     // RA4, IA4
  RA8 = 0x3,     // RA8, IA8 (and possibly Z16)
  RGB565 = 0x4,
  RGB5A3 = 0x5,
  RGBA8 = 0x6,  // RGBA8, Z24
  A8 = 0x7,
  R8 = 0x8,   // R8, I8, Z8H
  G8 = 0x9,   // G8, Z8M
  B8 = 0xA,   // B8, Z8L
  RG8 = 0xB,  // RG8, Z16R (G and R swapped)
  GB8 = 0xC,  // GB8, Z16L

  // Not a GX value; marks external framebuffer (YUYV) copies, see TextureFormat::XFB.
  XFB = 0xF,
};
template <>
struct fmt::formatter<EFBCopyFormat> : EnumFormatter<EFBCopyFormat::GB8>
{
  static constexpr array_type names = {
      "R4/I4/Z4",  "R8/I8/Z8H (?)", "RA4/IA4", "RA8/IA8 (Z16 too?)",
      "RGB565",    "RGB5A3",        "RGBA8",   "A8",
      "R8/I8/Z8H", "G8/Z8M",        "B8/Z8L",  "RG8/Z16R (Note: G and R are reversed)",
      "GB8/Z16L",
  };
  constexpr formatter() : EnumFormatter(names) {}
};

// Palette entry formats (TX_SETTLUT format field).
enum class TLUTFormat
{
  IA8 = 0x0,
  RGB565 = 0x1,
  RGB5A3 = 0x2,
};
template <>
struct fmt::formatter<TLUTFormat> : EnumFormatter<TLUTFormat::RGB5A3>
{
  constexpr formatter() : EnumFormatter({"IA8", "RGB565", "RGB5A3"}) {}
};

static inline bool IsValidTLUTFormat(TLUTFormat tlutfmt)
{
  return tlutfmt == TLUTFormat::IA8 || tlutfmt == TLUTFormat::RGB565 ||
         tlutfmt == TLUTFormat::RGB5A3;
}

// True when texels written in one format can be read in the other without any conversion:
// the 4-bit and 8-bit intensity formats share their layout with C4 and C8.
static inline bool IsCompatibleTextureFormat(TextureFormat from_format, TextureFormat to_format)
{
  if (from_format == to_format)
    return true;

  const auto is_4bit = [](TextureFormat f) {
    return f == TextureFormat::I4 || f == TextureFormat::C4;
  };
  const auto is_8bit = [](TextureFormat f) {
    return f == TextureFormat::I8 || f == TextureFormat::C8;
  };
  if (is_4bit(from_format))
    return is_4bit(to_format);
  if (is_8bit(from_format))
    return is_8bit(to_format);
  return false;
}

// True when a VRAM copy in one format can be redrawn as the other on the GPU. Only formats
// with the same block width qualify: the 8-bit pair I8/IA4 and the 16-bit group
// IA8/RGB565/RGB5A3.
static inline bool CanReinterpretTextureOnGPU(TextureFormat from_format, TextureFormat to_format)
{
  const auto in_8bit_group = [](TextureFormat f) {
    return f == TextureFormat::I8 || f == TextureFormat::IA4;
  };
  const auto in_16bit_group = [](TextureFormat f) {
    return f == TextureFormat::IA8 || f == TextureFormat::RGB565 || f == TextureFormat::RGB5A3;
  };
  if (in_8bit_group(from_format))
    return in_8bit_group(to_format);
  if (in_16bit_group(from_format))
    return in_16bit_group(to_format);
  return false;
}

inline std::span<u8> TexDecoder_GetTmemSpan(size_t offset = 0)
{
  return Common::SafeSubspan(std::span<u8>(s_tex_mem), offset);
}

int TexDecoder_GetTexelSizeInNibbles(TextureFormat format);
int TexDecoder_GetTextureSizeInBytes(int width, int height, TextureFormat format);
int TexDecoder_GetBlockWidthInTexels(TextureFormat format);
int TexDecoder_GetBlockHeightInTexels(TextureFormat format);
int TexDecoder_GetEFBCopyBlockWidthInTexels(EFBCopyFormat format);
int TexDecoder_GetEFBCopyBlockHeightInTexels(EFBCopyFormat format);
int TexDecoder_GetPaletteSize(TextureFormat fmt);
TextureFormat TexDecoder_GetEFBCopyBaseFormat(EFBCopyFormat format);

// Decodes a whole tiled texture to linear RGBA8 (R in the lowest byte). width and height
// must already be rounded up to the format's block size.
void TexDecoder_Decode(u8* dst, const u8* src, int width, int height, TextureFormat texformat,
                       const u8* tlut, TLUTFormat tlutfmt);
// Decodes an RGBA8 texture preloaded into TMEM, whose AR and GB halves live in the two banks.
void TexDecoder_DecodeRGBA8FromTmem(u8* dst, const u8* src_ar, const u8* src_gb, int width,
                                    int height);
// Single-texel decoders, used by the software renderer's sampler.
void TexDecoder_DecodeTexel(u8* dst, std::span<const u8> src, int s, int t, int imageWidth,
                            TextureFormat texformat, std::span<const u8> tlut, TLUTFormat tlutfmt);
void TexDecoder_DecodeTexelRGBA8FromTmem(u8* dst, std::span<const u8> src_ar,
                                         std::span<const u8> src_gb, int s, int t, int imageWidth);
void TexDecoder_DecodeTexelRGBA8FromTmem(u8* dst, const u8* src_ar, const u8* src_gb, int s, int t,
                                         int imageWidth);
// Converts YUYV rows (stride in bytes) to RGBA8.
void TexDecoder_DecodeXFB(u8* dst, const u8* src, u32 width, u32 height, u32 stride);

// The debug overlay that prints each texture's format name into its top-left corner (or
// centre).
void TexDecoder_SetTexFmtOverlayOptions(bool enable, bool center);

// The bulk decoder behind TexDecoder_Decode, implemented in TextureDecoder_x64.cpp.
void _TexDecoder_DecodeImpl(u32* dst, const u8* src, int width, int height, TextureFormat texformat,
                            const u8* tlut, TLUTFormat tlutfmt);
