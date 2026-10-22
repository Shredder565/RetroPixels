// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/TextureEncoder.h"

#include <cstddef>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Software/SWEfbInterface.h"
#include "VideoBackends/Software/SWTexture.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/LookUpTables.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VideoCommon.h"

namespace TextureEncoder
{
namespace
{
constexpr s64 EFB_BYTES = s64{EFB_WIDTH} * EFB_HEIGHT * 6;
constexpr s64 EFB_ROW_BYTES = s64{EFB_WIDTH} * 3;

// One source texel as 8-bit components.
struct Texel
{
  u8 r;
  u8 g;
  u8 b;
  u8 a;
};

// How the EFB data is read: 6 bits per component plus alpha (RGBA6), 8 bits per component
// without alpha (RGB8, also used for RGB565), or a 24-bit depth value read as R = bits 16-23,
// G = 8-15, B = 0-7.
enum class SourceKind
{
  RGBA6,
  RGB8,
  Depth,
};

struct SourceReader
{
  const u8* efb;  // start of the EFB storage
  SourceKind kind;
  bool box_filter;  // half-scale copies average 2x2 pixels
  bool swap_rb;     // half-scale depth copies to RGBA8/RG8/GB8 take R and B the other way round

  u8 Byte(s64 offset) const
  {
    // The copy area is block aligned and can reach past the EFB; that reads as zero.
    return offset >= 0 && offset < EFB_BYTES ? efb[offset] : 0;
  }

  u32 Pixel(s64 offset) const
  {
    return Byte(offset) | (u32{Byte(offset + 1)} << 8) | (u32{Byte(offset + 2)} << 16);
  }

  Texel Read(s64 offset) const
  {
    Texel out;
    if (kind == SourceKind::RGBA6)
    {
      if (!box_filter)
      {
        const u32 v = Pixel(offset);
        out.r = Convert6To8((v >> 18) & 0x3f);
        out.g = Convert6To8((v >> 12) & 0x3f);
        out.b = Convert6To8((v >> 6) & 0x3f);
        out.a = Convert6To8(v & 0x3f);
        return out;
      }
      // Sum the four 6-bit values (at most 252) and stretch the sum to 8 bits.
      u32 sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
      for (const s64 step : {s64{0}, s64{3}, EFB_ROW_BYTES, EFB_ROW_BYTES + 3})
      {
        const u32 v = Pixel(offset + step);
        sum_r += (v >> 18) & 0x3f;
        sum_g += (v >> 12) & 0x3f;
        sum_b += (v >> 6) & 0x3f;
        sum_a += v & 0x3f;
      }
      out.r = static_cast<u8>(sum_r + (sum_r >> 6));
      out.g = static_cast<u8>(sum_g + (sum_g >> 6));
      out.b = static_cast<u8>(sum_b + (sum_b >> 6));
      out.a = static_cast<u8>(sum_a + (sum_a >> 6));
      return out;
    }

    u32 hi, mid, lo;
    if (!box_filter)
    {
      hi = Byte(offset + 2);
      mid = Byte(offset + 1);
      lo = Byte(offset);
    }
    else
    {
      hi = mid = lo = 0;
      for (const s64 step : {s64{0}, s64{3}, EFB_ROW_BYTES, EFB_ROW_BYTES + 3})
      {
        hi += Byte(offset + step + 2);
        mid += Byte(offset + step + 1);
        lo += Byte(offset + step);
      }
      hi >>= 2;
      mid >>= 2;
      lo >>= 2;
    }
    out.r = static_cast<u8>(swap_rb ? lo : hi);
    out.g = static_cast<u8>(mid);
    out.b = static_cast<u8>(swap_rb ? hi : lo);
    out.a = 0xff;
    return out;
  }
};

// Luma from RGB, scaled by 256, with the +16 offset.
u8 Intensity(const Texel& t)
{
  return static_cast<u8>(static_cast<u16>(4096 + 66 * t.r + 129 * t.g + 25 * t.b) >> 8);
}

void PutBigEndian16(u8* dst, u32 value)
{
  dst[0] = static_cast<u8>(value >> 8);
  dst[1] = static_cast<u8>(value);
}

// Shapes of the destination tiles.
enum class Tile
{
  Nibble,   // 8x8 texels of 4 bits
  Byte,     // 8x4 texels of 8 bits
  Short,    // 4x4 texels of 16 bits
  TwoPass,  // 4x4 texels of 32 bits, stored as an AR tile followed by a GB tile
};

struct TileShape
{
  Tile tile;
  u32 width_log2;
  u32 height_log2;
};

bool ShapeOf(EFBCopyFormat format, TileShape* shape)
{
  switch (format)
  {
  case EFBCopyFormat::R4:
    *shape = {Tile::Nibble, 3, 3};
    return true;
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
  case EFBCopyFormat::RA4:
  case EFBCopyFormat::A8:
  case EFBCopyFormat::G8:
  case EFBCopyFormat::B8:
    *shape = {Tile::Byte, 3, 2};
    return true;
  case EFBCopyFormat::RA8:
  case EFBCopyFormat::RGB565:
  case EFBCopyFormat::RGB5A3:
  case EFBCopyFormat::RG8:
  case EFBCopyFormat::GB8:
    *shape = {Tile::Short, 2, 2};
    return true;
  case EFBCopyFormat::RGBA8:
    *shape = {Tile::TwoPass, 2, 2};
    return true;
  default:
    return false;
  }
}

// Depth copies support the formats that take whole bytes of the depth value.
bool DepthSupports(EFBCopyFormat format)
{
  switch (format)
  {
  case EFBCopyFormat::R4:
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
  case EFBCopyFormat::RGBA8:
  case EFBCopyFormat::G8:
  case EFBCopyFormat::B8:
  case EFBCopyFormat::RG8:
  case EFBCopyFormat::GB8:
    return true;
  default:
    return false;
  }
}

// Writes one texel of a byte or short format.
void PutTexel(EFBCopyFormat format, const Texel& t, bool yuv, u8* dst)
{
  const u8 luma_or_red = yuv ? Intensity(t) : t.r;
  switch (format)
  {
  case EFBCopyFormat::R8_0x1:
  case EFBCopyFormat::R8:
    dst[0] = luma_or_red;
    break;
  case EFBCopyFormat::RA4:
    dst[0] = static_cast<u8>((t.a & 0xf0) | (luma_or_red >> 4));
    break;
  case EFBCopyFormat::A8:
    dst[0] = t.a;
    break;
  case EFBCopyFormat::G8:
    dst[0] = t.g;
    break;
  case EFBCopyFormat::B8:
    dst[0] = t.b;
    break;
  case EFBCopyFormat::RA8:
    dst[0] = t.a;
    dst[1] = luma_or_red;
    break;
  case EFBCopyFormat::RG8:
    dst[0] = t.g;
    dst[1] = t.r;
    break;
  case EFBCopyFormat::GB8:
    dst[0] = t.b;
    dst[1] = t.g;
    break;
  case EFBCopyFormat::RGB565:
    PutBigEndian16(dst, ((u32{t.r} << 8) & 0xf800u) | ((u32{t.g} << 3) & 0x07e0u) |
                            ((u32{t.b} >> 3) & 0x001fu));
    break;
  case EFBCopyFormat::RGB5A3:
    // Opaque enough texels use 5:5:5, the others 3:4:4:4.
    if (t.a >= 224)
    {
      PutBigEndian16(dst, 0x8000u | ((u32{t.r} << 7) & 0x7c00u) | ((u32{t.g} << 2) & 0x03e0u) |
                              ((u32{t.b} >> 3) & 0x001fu));
    }
    else
    {
      PutBigEndian16(dst, ((u32{t.a} << 7) & 0x7000u) | ((u32{t.r} << 4) & 0x0f00u) |
                              (u32{t.g} & 0x00f0u) | ((u32{t.b} >> 4) & 0x000fu));
    }
    break;
  default:
    break;
  }
}

// Walks the copy rectangle in tile order, reading the EFB through `reader` and writing tiles to
// `dst`. The source position advances the way the hardware's copy unit does: per texel by
// texel_step bytes, and between rows and tiles by multiples of row_step (both depend on the
// half-scale setting).
void EncodeTiles(u8* dst, const SourceReader& reader, s64 start, EFBCopyFormat format,
                 const TileShape& shape, bool yuv, bool scale_by_half)
{
  // The source size in the copy registers is one less than the number of pixels.
  const u32 half = bpmem.triggerEFBCopy.half_scale ? 1 : 0;
  const u32 width = bpmem.copyTexSrcWH.x >> half;
  const u32 height = bpmem.copyTexSrcWH.y >> half;
  const u32 tiles_x = static_cast<u16>((width >> shape.width_log2) + 1);
  const u32 tiles_y = static_cast<u16>((height >> shape.height_log2) + 1);
  const s64 tile_w = s64{1} << shape.width_log2;
  const s64 tile_h = s64{1} << shape.height_log2;
  const s64 row_step = s64{3} << half;
  const s64 texel_step = scale_by_half ? 6 : 3;

  // After a row of a tile: on to the next EFB row. After a tile: back up to the tile's first
  // row and on to the next tile. After a row of tiles: down to the next row of tiles.
  const s64 after_row = (EFB_WIDTH - tile_w) * row_step;
  const s64 after_tile = (tile_w - EFB_WIDTH * tile_h) * row_step;
  const s64 after_tile_row = (EFB_WIDTH * tile_h - tile_w * tiles_x) * row_step;
  // The stride register counts 32-byte cache lines (the shift wraps like the register's 32 bits).
  const s64 dst_row_pitch = static_cast<s32>(bpmem.copyDestStride << 5);

  // The RGB8 path writes constant alpha without reading anything.
  const bool reads_source = !(format == EFBCopyFormat::A8 && reader.kind != SourceKind::RGBA6);

  s64 src = start;
  u8* tile_row = dst;
  for (u32 ty = 0; ty < tiles_y; ++ty, tile_row += dst_row_pitch)
  {
    u8* out = tile_row;
    for (u32 tx = 0; tx < tiles_x; ++tx)
    {
      for (s64 row = 0; row < tile_h; ++row)
      {
        for (s64 col = 0; col < tile_w; ++col)
        {
          switch (shape.tile)
          {
          case Tile::Nibble:
          {
            // Two texels per byte, the first in the high nibble.
            const Texel first = reader.Read(src);
            src += texel_step;
            const Texel second = reader.Read(src);
            src += texel_step;
            const u8 hi = yuv ? Intensity(first) : first.r;
            const u8 lo = yuv ? Intensity(second) : second.r;
            *out++ = static_cast<u8>((hi & 0xf0) | (lo >> 4));
            ++col;
            break;
          }
          case Tile::TwoPass:
          {
            const Texel t = reader.Read(src);
            src += texel_step;
            out[0] = t.a;
            out[1] = t.r;
            out[32] = t.g;
            out[33] = t.b;
            out += 2;
            break;
          }
          case Tile::Byte:
          case Tile::Short:
            if (reads_source)
            {
              PutTexel(format, reader.Read(src), yuv, out);
              src += texel_step;
            }
            else
            {
              *out = 0xff;
            }
            out += shape.tile == Tile::Byte ? 1 : 2;
            break;
          }
        }
        src += after_row;
      }
      src += after_tile;
      if (shape.tile == Tile::TwoPass)
        out += 32;  // skip the GB half of the tile
    }
    src += after_tile_row;
  }
}

void EncodeEfbCopy(u8* dst, const EFBCopyParams& params, const MathUtil::Rectangle<int>& src_rect,
                   bool scale_by_half)
{
  SourceReader reader;
  reader.box_filter = scale_by_half;
  reader.swap_rb = false;
  bool yuv = params.yuv;
  switch (params.efb_format)
  {
  case PixelFormat::RGBA6_Z24:
    reader.kind = SourceKind::RGBA6;
    break;
  case PixelFormat::RGB8_Z24:
  case PixelFormat::RGB565_Z16:
    reader.kind = SourceKind::RGB8;
    break;
  case PixelFormat::Z24:
    reader.kind = SourceKind::Depth;
    yuv = false;
    break;
  default:
    return;
  }

  TileShape shape;
  const EFBCopyFormat format = params.copy_format;
  if (!ShapeOf(format, &shape) || (reader.kind == SourceKind::Depth && !DepthSupports(format)))
  {
    PanicAlertFmt("Unknown texture copy format: {}\n", format);
    return;
  }

  if (reader.kind == SourceKind::Depth && scale_by_half)
  {
    reader.swap_rb = format == EFBCopyFormat::RGBA8 || format == EFBCopyFormat::RG8 ||
                     format == EFBCopyFormat::GB8;
  }

  reader.efb = EfbInterface::GetPixelPointer(0, 0, false);
  const u8* first = EfbInterface::GetPixelPointer(static_cast<u16>(src_rect.left),
                                                  static_cast<u16>(src_rect.top), params.depth);
  EncodeTiles(dst, reader, first - reader.efb, format, shape, yuv, scale_by_half);
}
}  // namespace

void Encode(AbstractStagingTexture* dst, const EFBCopyParams& params, u32 native_width,
            u32 /*bytes_per_row*/, u32 /*num_blocks_y*/, u32 memory_stride,
            const MathUtil::Rectangle<int>& src_rect, bool scale_by_half, float y_scale,
            float gamma)
{
  // The encoders write rows tightly packed, as they would land in memory, while the staging
  // texture is wider (2560); give it the copy's stride so reading it back matches.
  ASSERT(memory_stride <= (dst->GetConfig().width * dst->GetTexelSize()));
  static_cast<SW::SWStagingTexture*>(dst)->SetMapStride(memory_stride);

  u8* const out = reinterpret_cast<u8*>(dst->GetMappedPointer());
  if (params.copy_format == EFBCopyFormat::XFB)
    EfbInterface::EncodeXFB(out, native_width, src_rect, y_scale, gamma);
  else
    EncodeEfbCopy(out, params, src_rect, scale_by_half);
}
}  // namespace TextureEncoder
