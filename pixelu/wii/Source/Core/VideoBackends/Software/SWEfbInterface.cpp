// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/SWEfbInterface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/Config/GraphicsSettings.h"

#include "VideoBackends/Software/CopyRegion.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/LookUpTables.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/VideoCommon.h"

namespace EfbInterface
{
namespace
{
// Byte positions within an A, B, G, R colour.
enum : size_t
{
  BYTE_A = 0,
  BYTE_B = 1,
  BYTE_G = 2,
  BYTE_R = 3,
};

constexpr u32 PLANE_BYTES = EFB_WIDTH * EFB_HEIGHT * 3;

std::array<u8, PLANE_BYTES * 2> s_efb{};
std::array<u32, PQ_NUM_MEMBERS> s_perf_counts{};
// The counters count 2x2 quads but are fed single pixels; every third pixel advances them.
std::array<u32, PQ_NUM_MEMBERS> s_quad_phase{};

u32 ColorOffset(u16 x, u16 y)
{
  return (x + y * EFB_WIDTH) * 3;
}

u32 DepthOffset(u16 x, u16 y)
{
  return PLANE_BYTES + (x + y * EFB_WIDTH) * 3;
}

u32 Load24(u32 offset)
{
  return u32{s_efb[offset]} | (u32{s_efb[offset + 1]} << 8) | (u32{s_efb[offset + 2]} << 16);
}

void Store24(u32 offset, u32 value)
{
  s_efb[offset] = static_cast<u8>(value);
  s_efb[offset + 1] = static_cast<u8>(value >> 8);
  s_efb[offset + 2] = static_cast<u8>(value >> 16);
}

void LogUnsupportedFormat()
{
  ERROR_LOG_FMT(VIDEO, "Unsupported pixel format: {}", bpmem.zcontrol.pixel_format);
}

// Writes the colour and/or alpha part of a pixel. RGBA6 keeps 6 bits per component (alpha in
// the low bits); the other colour formats keep RGB8 and have no alpha to write. RGB565 is
// stored as RGB8 for now.
void StoreColor(u32 offset, const u8* color, bool write_color, bool write_alpha)
{
  switch (bpmem.zcontrol.pixel_format)
  {
  case PixelFormat::RGB8_Z24:
  case PixelFormat::Z24:
  case PixelFormat::RGB565_Z16:
    if (write_color)
      Store24(offset, (u32{color[BYTE_R]} << 16) | (u32{color[BYTE_G]} << 8) | u32{color[BYTE_B]});
    break;
  case PixelFormat::RGBA6_Z24:
  {
    u32 packed = Load24(offset);
    if (write_color)
    {
      packed = (packed & 0x3f) | (u32(color[BYTE_B] >> 2) << 6) | (u32(color[BYTE_G] >> 2) << 12) |
               (u32(color[BYTE_R] >> 2) << 18);
    }
    if (write_alpha)
      packed = (packed & ~0x3fu) | u32(color[BYTE_A] >> 2);
    Store24(offset, packed);
    break;
  }
  default:
    LogUnsupportedFormat();
    break;
  }
}

// Reads a pixel's colour as an A, B, G, R word (formats without alpha read as opaque).
u32 ReadColor(u32 offset)
{
  const u32 packed = Load24(offset);
  switch (bpmem.zcontrol.pixel_format)
  {
  case PixelFormat::RGB8_Z24:
  case PixelFormat::Z24:
  case PixelFormat::RGB565_Z16:
    return (packed << 8) | 0xff;
  case PixelFormat::RGBA6_Z24:
    return u32(Convert6To8(packed & 0x3f)) | (u32(Convert6To8((packed >> 6) & 0x3f)) << 8) |
           (u32(Convert6To8((packed >> 12) & 0x3f)) << 16) |
           (u32(Convert6To8((packed >> 18) & 0x3f)) << 24);
  default:
    LogUnsupportedFormat();
    return 0;
  }
}

bool HasDepth24()
{
  switch (bpmem.zcontrol.pixel_format)
  {
  case PixelFormat::RGB8_Z24:
  case PixelFormat::RGBA6_Z24:
  case PixelFormat::Z24:
  case PixelFormat::RGB565_Z16:  // stored as 24 bits for now
    return true;
  default:
    LogUnsupportedFormat();
    return false;
  }
}

void StoreDepth(u32 offset, u32 depth)
{
  if (HasDepth24())
    Store24(offset, depth & 0x00ffffff);
}

u32 ReadDepth(u32 offset)
{
  return HasDepth24() ? Load24(offset) : 0;
}

// Blend weight of one channel, 0 to 255.
template <typename Factor>
u32 Weight(Factor factor, const u8* src, const u8* dst, size_t ch)
{
  // Both factor enums share this numbering; 2 and 3 are the "other" colour.
  switch (static_cast<u32>(factor))
  {
  case 0:  // zero
    return 0;
  case 1:  // one
    return 255;
  case 2:  // other colour
    return std::is_same_v<Factor, SrcBlendFactor> ? dst[ch] : src[ch];
  case 3:  // one minus other colour
    return 255u - (std::is_same_v<Factor, SrcBlendFactor> ? dst[ch] : src[ch]);
  case 4:  // source alpha
    return src[BYTE_A];
  case 5:
    return 255u - src[BYTE_A];
  case 6:  // destination alpha
    return dst[BYTE_A];
  case 7:
  default:
    return 255u - dst[BYTE_A];
  }
}

// result = (src * sw + dst * dw) / 256, with each weight stretched from 0-255 to 0-256.
void BlendAdd(const u8* src, u8* dst)
{
  std::array<u32, 4> src_w, dst_w;
  for (size_t ch = 0; ch < 4; ++ch)
  {
    src_w[ch] = Weight(bpmem.blendmode.src_factor.Value(), src, dst, ch);
    dst_w[ch] = Weight(bpmem.blendmode.dst_factor.Value(), src, dst, ch);
  }
  for (size_t ch = 0; ch < 4; ++ch)
  {
    const u32 sw = src_w[ch] + (src_w[ch] >> 7);
    const u32 dw = dst_w[ch] + (dst_w[ch] >> 7);
    dst[ch] = static_cast<u8>(std::min<u32>((src[ch] * sw + dst[ch] * dw) >> 8, 255));
  }
}

void BlendSubtract(const u8* src, u8* dst)
{
  for (size_t ch = 0; ch < 4; ++ch)
    dst[ch] = static_cast<u8>(std::max(dst[ch] - src[ch], 0));
}

// The sixteen logic ops, in GX order, are the sixteen truth tables: bit 0 of the op selects
// src AND dst, bit 1 src AND NOT dst, bit 2 NOT src AND dst, bit 3 NOT src AND NOT dst.
u32 LogicOpResult(u32 src, u32 dst, LogicOp op)
{
  const u32 table = static_cast<u32>(op);
  u32 out = 0;
  if (table & 1)
    out |= src & dst;
  if (table & 2)
    out |= src & ~dst;
  if (table & 4)
    out |= ~src & dst;
  if (table & 8)
    out |= ~src & ~dst;
  return out;
}

// 2x2 ordered dither applied before colours are cut to 6 bits.
void Dither(u16 x, u16 y, u8* color)
{
  if (!bpmem.blendmode.dither || bpmem.zcontrol.pixel_format != PixelFormat::RGBA6_Z24)
    return;
  static constexpr std::array<std::array<u8, 2>, 2> BAYER{{{0, 2}, {3, 1}}};
  const u8 bias = BAYER[y & 1][x & 1];
  // Alpha is not dithered.
  for (size_t ch : {BYTE_B, BYTE_G, BYTE_R})
    color[ch] = static_cast<u8>(((color[ch] - (color[ch] >> 6)) + bias) & 0xfc);
}

void WriteMasked(u16 x, u16 y, u32 offset, u8* color, bool dither)
{
  if (bpmem.blendmode.color_update)
  {
    if (dither)
      Dither(x, y, color);
    StoreColor(offset, color, true, bpmem.blendmode.alpha_update);
  }
  else if (bpmem.blendmode.alpha_update)
  {
    StoreColor(offset, color, false, true);
  }
}

// XFB conversion of one EFB pixel (A, B, G, R word), before chroma subsampling.
struct YuvSample
{
  u8 y;
  s8 u;
  s8 v;
};

// Deflicker / copy filter: a vertical 3-tap filter whose 7 coefficients (summing to 64 for
// unchanged brightness) weight the row above (0-1), the row itself (2-4) and the row below (5-6).
// Multisampling is not modelled. The result is an A, B, G, R word with alpha 0.
u32 CopyFilter(u32 above, u32 center, u32 below, const std::array<u8, 7>& coef, bool disabled)
{
  const int top = coef[0] + coef[1];
  const int mid = coef[2] + coef[3] + coef[4];
  const int bottom = coef[5] + coef[6];
  u32 out = 0;
  for (int shift = 8; shift <= 24; shift += 8)
  {
    const int a = (above >> shift) & 0xff;
    const int c = (center >> shift) & 0xff;
    const int b = (below >> shift) & 0xff;
    // With the filter disabled (a debugging aid) all the weight goes to the pixel itself.
    const int sum = disabled ? c * (top + mid + bottom) : a * top + c * mid + b * bottom;
    // Larger results saturate; this matches tests but is not confirmed on hardware.
    out |= static_cast<u32>(std::min(255, sum >> 6)) << shift;
  }
  return out;
}

u32 ApplyGamma(u32 color, float inverse_gamma)
{
  u32 out = 0;
  for (int shift = 8; shift <= 24; shift += 8)
  {
    const float level = static_cast<float>((color >> shift) & 0xff) / 255.0f;
    const float corrected = std::clamp(std::pow(level, inverse_gamma) * 255.0f, 0.0f, 255.0f);
    out |= static_cast<u32>(static_cast<u8>(corrected)) << shift;
  }
  return out;
}

// BT.601 RGB to YCbCr with the coefficients measured on hardware, scaled by 256 and rounded.
// The 16 and 128 offsets are added when the samples are packed.
YuvSample ToYuv(u32 color)
{
  const int r = static_cast<u8>(color >> 24);
  const int g = static_cast<u8>(color >> 16);
  const int b = static_cast<u8>(color >> 8);
  const u16 y = static_cast<u16>(66 * r + 129 * g + 25 * b);
  const s16 u = static_cast<s16>(-38 * r - 74 * g + 112 * b);
  const s16 v = static_cast<s16>(112 * r - 94 * g - 18 * b);
  const auto round8 = [](int value) { return (value >> 8) + ((value >> 7) & 1); };
  return {static_cast<u8>(round8(y)), static_cast<s8>(round8(u)), static_cast<s8>(round8(v))};
}
}  // namespace

void BlendTev(u16 x, u16 y, u8* color)
{
  const u32 offset = ColorOffset(x, y);
  const u32 current = ReadColor(offset);

  std::array<u8, 4> result;
  std::memcpy(result.data(), &current, sizeof(current));

  if (bpmem.blendmode.blend_enable)
  {
    if (bpmem.blendmode.subtract)
      BlendSubtract(color, result.data());
    else
      BlendAdd(color, result.data());
  }
  else if (bpmem.blendmode.logic_op_enable)
  {
    u32 src;
    std::memcpy(&src, color, sizeof(src));
    const u32 mixed = LogicOpResult(src, current, bpmem.blendmode.logic_mode);
    std::memcpy(result.data(), &mixed, sizeof(mixed));
  }
  else
  {
    std::memcpy(result.data(), color, result.size());
  }

  if (bpmem.dstalpha.enable)
    result[BYTE_A] = static_cast<u8>(bpmem.dstalpha.alpha);

  WriteMasked(x, y, offset, result.data(), true);
}

void SetColor(u16 x, u16 y, u8* color)
{
  WriteMasked(x, y, ColorOffset(x, y), color, false);
}

void SetDepth(u16 x, u16 y, u32 depth)
{
  if (bpmem.zmode.update_enable)
    StoreDepth(DepthOffset(x, y), depth);
}

u8* GetPixelPointer(u16 x, u16 y, bool depth)
{
  return &s_efb[depth ? DepthOffset(x, y) : ColorOffset(x, y)];
}

void EncodeXFB(u8* xfb_in_ram, u32 memory_stride, const MathUtil::Rectangle<int>& source_rect,
               float y_scale, float gamma)
{
  if (!xfb_in_ram)
  {
    WARN_LOG_FMT(VIDEO, "Tried to copy to invalid XFB address");
    return;
  }

  const int left = source_rect.left;
  const int right = source_rect.right;
  // Pixels are paired as (Y, Cb) (Y, Cr), so an odd start or width encodes wrongly.
  if (left & 1 || right & 1)
    WARN_LOG_FMT(VIDEO, "Trying to copy XFB to from unaligned EFB source");

  const bool clamp_top = bpmem.triggerEFBCopy.clamp_top;
  const bool clamp_bottom = bpmem.triggerEFBCopy.clamp_bottom;
  const auto coefficients = bpmem.copyfilter.GetCoefficients();
  const bool filter_disabled = Config::Get(Config::GFX_ENHANCE_DISABLE_COPY_FILTER);
  const float inverse_gamma = 1.0f / gamma;

  // Rows are encoded into this buffer at their EFB x position, memory_stride pixels apart, then
  // scaled vertically into the XFB. It keeps its contents between copies.
  static std::vector<yuv422_packed> encoded(EFB_WIDTH * EFB_HEIGHT);

  // One row of YUV 4:4:4 samples. Entry i is EFB column left + i - 1; entries 0 and
  // right + 1 repeat the edge samples for the chroma filter.
  std::array<YuvSample, EFB_WIDTH + 2> row{};

  yuv422_packed* out_row = encoded.data();
  for (int y = source_rect.top; y < source_rect.bottom; ++y)
  {
    // Without clamping, rows outside the source come from the rest of the EFB here. (Hardware
    // reads whatever the bus returns past the EFB edges.)
    const int first_row = clamp_top ? source_rect.top : 0;
    const int last_row = (clamp_bottom ? source_rect.bottom : static_cast<int>(EFB_HEIGHT)) - 1;
    const u16 above = static_cast<u16>(std::max(first_row, y - 1));
    const u16 below = static_cast<u16>(std::min(last_row, y + 1));

    for (int x = left; x < right; ++x)
    {
      const u16 ex = static_cast<u16>(x);
      const u32 filtered = CopyFilter(
          ReadColor(ColorOffset(ex, above)), ReadColor(ColorOffset(ex, static_cast<u16>(y))),
          ReadColor(ColorOffset(ex, below)), coefficients, filter_disabled);
      row[static_cast<size_t>(x - left + 1)] = ToYuv(ApplyGamma(filtered, inverse_gamma));
    }
    row[0] = row[1];
    row[static_cast<size_t>(right + 1)] = row[static_cast<size_t>(right)];

    // 4:2:2: even pixels carry Cb, odd pixels Cr, each filtered 1/4 1/2 1/4 around the even
    // pixel.
    for (int x = left; x < right; x += 2)
    {
      const size_t i = static_cast<size_t>(x - left + 1);
      const int cb = (row[i - 1].u + (row[i].u * 2) + row[i + 1].u) >> 2;
      const int cr = (row[i - 1].v + (row[i].v * 2) + row[i + 1].v) >> 2;
      out_row[x].Y = static_cast<u8>(row[i].y + 16);
      out_row[x].UV = static_cast<u8>(128 + cb);
      out_row[x + 1].Y = static_cast<u8>(row[i + 1].y + 16);
      out_row[x + 1].UV = static_cast<u8>(128 + cr);
    }
    out_row += memory_stride;
  }

  const int width = source_rect.GetWidth();
  const int height = source_rect.GetHeight();
  SW::CopyRegion(encoded.data(), width, height, reinterpret_cast<yuv422_packed*>(xfb_in_ram), width,
                 static_cast<int>(static_cast<float>(height) * y_scale));
}

bool ZCompare(u16 x, u16 y, u32 z)
{
  const u32 offset = DepthOffset(x, y);
  const u32 stored = ReadDepth(offset);

  bool pass = false;
  switch (bpmem.zmode.func)
  {
  case CompareMode::Never:
    pass = false;
    break;
  case CompareMode::Less:
    pass = z < stored;
    break;
  case CompareMode::Equal:
    pass = z == stored;
    break;
  case CompareMode::LEqual:
    pass = z <= stored;
    break;
  case CompareMode::Greater:
    pass = z > stored;
    break;
  case CompareMode::NEqual:
    pass = z != stored;
    break;
  case CompareMode::GEqual:
    pass = z >= stored;
    break;
  case CompareMode::Always:
    pass = true;
    break;
  }

  if (pass && bpmem.zmode.update_enable)
    StoreDepth(offset, z);
  return pass;
}

u32 GetPerfQueryResult(PerfQueryType type)
{
  return s_perf_counts[type];
}

void ResetPerfQuery()
{
  s_perf_counts.fill(0);
}

void IncPerfCounterQuadCount(PerfQueryType type)
{
  if (++s_quad_phase[type] != 3)
    return;
  s_quad_phase[type] = 0;
  ++s_perf_counts[type];
}

u32 PeekColorWord(u16 x, u16 y)
{
  return ReadColor(ColorOffset(x, y));
}

u32 PeekDepthWord(u16 x, u16 y)
{
  return ReadDepth(DepthOffset(x, y));
}
}  // namespace EfbInterface

namespace SW
{
void SWEFBInterface::ReinterpretPixelData(EFBReinterpretType)
{
}

void SWEFBInterface::PokeColor(u16, u16, u32)
{
}

void SWEFBInterface::PokeDepth(u16, u16, u32)
{
}

u32 SWEFBInterface::PeekColorInternal(u16 x, u16 y)
{
  // A, B, G, R bytes to an ARGB word.
  const u32 abgr = EfbInterface::PeekColorWord(x, y);
  return (abgr >> 8) | ((abgr & 0xff) << 24);
}

u32 SWEFBInterface::PeekDepthInternal(u16 x, u16 y)
{
  return EfbInterface::PeekDepthWord(x, y);
}
}  // namespace SW
