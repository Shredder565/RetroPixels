// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/VertexLoader_Color.h"

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoaderUtils.h"

namespace
{
using VertexData::Source;

constexpr u32 OPAQUE_ALPHA = 0xff000000;

// Widens an n-bit channel to 8 bits by repeating its top bits below it.
constexpr u32 Widen5(u32 v)
{
  return (v << 3) | (v >> 2);
}
constexpr u32 Widen6(u32 v)
{
  return (v << 2) | (v >> 4);
}
constexpr u32 Widen4(u32 v)
{
  return v * 0x11;
}

constexpr u32 PackRGBA(u32 r, u32 g, u32 b, u32 a)
{
  return r | (g << 8) | (b << 16) | (a << 24);
}

// Each decoder turns the colour bytes at p into the host RGBA8 word.

// 16-bit big-endian RRRRRGGG GGGBBBBB.
u32 DecodeRGB565(const u8* p)
{
  const u32 v = VertexData::LoadBE<u16>(p);
  return PackRGBA(Widen5(v >> 11), Widen6((v >> 5) & 0x3f), Widen5(v & 0x1f), 0xff);
}

// R, G, B bytes. Four bytes are read, and the fourth is replaced by an opaque alpha.
u32 DecodeRGB888(const u8* p)
{
  return VertexData::LoadRaw<u32>(p) | OPAQUE_ALPHA;
}

// Bytes RRRRGGGG BBBBAAAA.
u32 DecodeRGBA4444(const u8* p)
{
  return PackRGBA(Widen4(p[0] >> 4), Widen4(p[0] & 0xfu), Widen4(p[1] >> 4), Widen4(p[1] & 0xfu));
}

// 24-bit big-endian RRRRRRGG GGGGBBBB BBAAAAAA.
u32 DecodeRGBA6666(const u8* p)
{
  const u32 v = Common::swap24(p);
  return PackRGBA(Widen6((v >> 18) & 0x3f), Widen6((v >> 12) & 0x3f), Widen6((v >> 6) & 0x3f),
                  Widen6(v & 0x3f));
}

// R, G, B, A bytes as they are.
u32 DecodeRGBA8888(const u8* p)
{
  return VertexData::LoadRaw<u32>(p);
}

template <u32 (*Decode)(const u8*), u32 InlineBytes, Source S>
void ReadColor(VertexLoaderCursor& cursor, const VertexLoaderStep& step)
{
  const u8* data;
  if constexpr (VertexData::IsIndexSource<S>)
    data = VertexData::ArrayEntry(step.array, VertexData::TakeIndex<S>(cursor));
  else
    data = VertexData::TakeInline<S>(cursor, InlineBytes);
  VertexData::Emit(cursor.dst, Decode(data));
}

template <Source S>
VertexLoaderStep::Fn PickFormat(ColorFormat format)
{
  switch (format)
  {
  case ColorFormat::RGB565:
    return ReadColor<DecodeRGB565, 2, S>;
  case ColorFormat::RGB888:
    return ReadColor<DecodeRGB888, 3, S>;
  case ColorFormat::RGB888x:
    return ReadColor<DecodeRGB888, 4, S>;
  case ColorFormat::RGBA4444:
    return ReadColor<DecodeRGBA4444, 2, S>;
  case ColorFormat::RGBA6666:
    return ReadColor<DecodeRGBA6666, 3, S>;
  case ColorFormat::RGBA8888:
    return ReadColor<DecodeRGBA8888, 4, S>;
  default:
    return nullptr;
  }
}
}  // namespace

VertexLoaderStep::Fn VertexLoader_Color::GetReader(VertexComponentFormat type, ColorFormat format)
{
  if (format > ColorFormat::RGBA8888)
  {
    PanicAlertFmt("Invalid color format {}", format);
    return nullptr;
  }
  switch (type)
  {
  case VertexComponentFormat::Direct:
    return PickFormat<Source::Inline>(format);
  case VertexComponentFormat::Index8:
    return PickFormat<Source::Index8>(format);
  case VertexComponentFormat::Index16:
    return PickFormat<Source::Index16>(format);
  default:
    return nullptr;
  }
}
