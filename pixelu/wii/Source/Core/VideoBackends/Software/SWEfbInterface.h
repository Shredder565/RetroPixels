// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"
#include "VideoCommon/EFBInterface.h"
#include "VideoCommon/PerfQueryBase.h"

// The software renderer's embedded framebuffer: a 640x528 colour plane followed by a 640x528
// depth plane, each pixel 24 bits packed in three bytes, as the pixel engine stores them.
namespace EfbInterface
{
// One XFB pixel: luma and alternately Cb or Cr.
#pragma pack(push, 1)
struct yuv422_packed
{
  u8 Y;
  u8 UV;
};
#pragma pack(pop)

// Colours passed to and from these functions are four bytes in the order A, B, G, R (a
// little-endian RGBA8 word).

// Blends a shaded pixel into the EFB with the current blend, logic op, dither and write masks.
void BlendTev(u16 x, u16 y, u8* color);

// Depth test against the stored value; a passing z is written when depth updates are enabled.
bool ZCompare(u16 x, u16 y, u32 z);

// Raw writes, honouring the write masks and the pixel format (used by EFB clears).
void SetColor(u16 x, u16 y, u8* color);
void SetDepth(u16 x, u16 y, u32 depth);

// Address of a pixel's three bytes in the colour or depth plane.
u8* GetPixelPointer(u16 x, u16 y, bool depth);

// Copies an EFB rectangle to the XFB in YUV 4:2:2, applying the copy filter and gamma.
void EncodeXFB(u8* xfb_in_ram, u32 memory_stride, const MathUtil::Rectangle<int>& source_rect,
               float y_scale, float gamma);

u32 GetPerfQueryResult(PerfQueryType type);
void ResetPerfQuery();
void IncPerfCounterQuadCount(PerfQueryType type);
}  // namespace EfbInterface

namespace SW
{
class SWEFBInterface final : public EFBInterfaceBase
{
  void ReinterpretPixelData(EFBReinterpretType convtype) override;

  void PokeColor(u16 x, u16 y, u32 color) override;
  void PokeDepth(u16 x, u16 y, u32 depth) override;

  u32 PeekColorInternal(u16 x, u16 y) override;
  u32 PeekDepthInternal(u16 x, u16 y) override;
};
}  // namespace SW
