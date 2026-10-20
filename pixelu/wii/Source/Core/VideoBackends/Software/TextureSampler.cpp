// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/TextureSampler.h"

#include <algorithm>
#include <array>
#include <span>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Common/SpanUtils.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/TextureDecoder.h"

namespace TextureSampler
{
namespace
{
// Brings a texel coordinate into the image. As YAGCD notes, repeat and mirror work by masking
// with size - 1, so they only wrap cleanly for power-of-two sizes; hardware behaves the same
// way with other sizes.
int WrapTexel(int coord, WrapMode mode, int size)
{
  switch (mode)
  {
  case WrapMode::Clamp:
    return std::clamp(coord, 0, size - 1);
  case WrapMode::Repeat:
    return coord & (size - 1);
  case WrapMode::Mirror:
    // Odd repeats (the size bit set) run backwards.
    return ((coord & size) != 0 ? ~coord : coord) & (size - 1);
  default:
    // Mode 3 acts like clamp on hardware.
    PanicAlertFmt("Invalid wrap mode: {}", mode);
    return std::clamp(coord, 0, size - 1);
  }
}

// One mip level of the texture bound to a texture map.
struct MipImage
{
  std::span<const u8> data;
  std::span<const u8> data_odd;  // second half of RGBA8 data held in TMEM
  std::span<const u8> tlut;
  TextureFormat format;
  TLUTFormat tlut_format;
  bool rgba8_from_tmem;
  int width_minus_1;
  int height_minus_1;
  WrapMode wrap_s;
  WrapMode wrap_t;

  void Fetch(int s, int t, u8* out) const
  {
    if (rgba8_from_tmem)
      TexDecoder_DecodeTexelRGBA8FromTmem(out, data, data_odd, s, t, width_minus_1);
    else
      TexDecoder_DecodeTexel(out, data, s, t, width_minus_1, format, tlut, tlut_format);
  }
};

// Finds level `mip` of the texture and scales s and t to it.
MipImage LocateMip(const TexUnit& unit, int mip, s32* s, s32* t)
{
  MipImage image;
  image.format = unit.texImage0.format;
  image.tlut_format = unit.texTlut.tlut_format;
  image.wrap_s = unit.texMode0.wrap_s;
  image.wrap_t = unit.texMode0.wrap_t;
  image.width_minus_1 = static_cast<int>(unit.texImage0.width);
  image.height_minus_1 = static_cast<int>(unit.texImage0.height);

  const bool in_tmem = unit.texImage1.cache_manually_managed;
  image.rgba8_from_tmem = in_tmem && image.format == TextureFormat::RGBA8;
  if (in_tmem)
  {
    image.data = TexDecoder_GetTmemSpan(unit.texImage1.tmem_even * TMEM_LINE_SIZE);
    if (image.format == TextureFormat::RGBA8)
      image.data_odd = TexDecoder_GetTmemSpan(unit.texImage2.tmem_odd * TMEM_LINE_SIZE);
  }
  else
  {
    auto& memory = Core::System::GetInstance().GetMemory();
    image.data = memory.GetSpanForAddress(unit.texImage3.image_base << 5);
  }
  image.tlut = TexDecoder_GetTmemSpan(unit.texTlut.tmem_offset << 9);

  if (mip == 0)
    return image;

  image.width_minus_1 >>= mip;
  image.height_minus_1 >>= mip;
  *s >>= mip;
  *t >>= mip;

  // Skip the larger levels. Each is stored in whole blocks, so a level smaller than a block
  // still takes a block.
  const int block_w = TexDecoder_GetBlockWidthInTexels(image.format);
  const int block_h = TexDecoder_GetBlockHeightInTexels(image.format);
  const int nibbles = TexDecoder_GetTexelSizeInNibbles(image.format);
  int level_w = static_cast<int>(unit.texImage0.width) + 1;
  int level_h = static_cast<int>(unit.texImage0.height) + 1;
  for (int level = 0; level < mip; ++level)
  {
    level_w = std::max(level_w, block_w);
    level_h = std::max(level_h, block_h);
    const u32 level_bytes = static_cast<u32>(level_w * level_h * nibbles) >> 1;
    image.data = Common::SafeSubspan(image.data, level_bytes);
    level_w >>= 1;
    level_h >>= 1;
  }
  return image;
}

// Accumulates weight * texel into a four-channel sum.
void Accumulate(std::array<u32, 4>& sum, const std::array<u8, 4>& texel, u32 weight)
{
  for (size_t i = 0; i < 4; ++i)
    sum[i] += texel[i] * weight;
}
}  // namespace

void Sample(s32 s, s32 t, s32 lod, bool linear, u8 texmap, u8* sample)
{
  const TexMode0& mode = bpmem.tex.GetUnit(texmap).texMode0;

  // lod is s28.4: the integer part picks the level, the fraction blends towards the next one.
  const s32 fraction = lod & 0xf;
  int level = 0;
  bool blend_levels = false;
  if (lod > 0 && mode.mipmap_filter != MipMode::None)
  {
    level = lod >> 4;
    blend_levels = fraction != 0 && mode.mipmap_filter == MipMode::Linear;
    // Nearest-level filtering rounds to the closer level.
    if (mode.mipmap_filter == MipMode::Point && fraction >= 8)
      ++level;
  }

  if (!blend_levels)
  {
    SampleMip(s, t, level, linear, texmap, sample);
    return;
  }

  std::array<u8, 4> lower, upper;
  SampleMip(s, t, level, linear, texmap, lower.data());
  SampleMip(s, t, level + 1, linear, texmap, upper.data());
  std::array<u32, 4> sum{};
  Accumulate(sum, lower, static_cast<u32>(16 - fraction));
  Accumulate(sum, upper, static_cast<u32>(fraction));
  for (size_t i = 0; i < 4; ++i)
    sample[i] = static_cast<u8>(sum[i] >> 4);
}

void SampleMip(s32 s, s32 t, s32 mip, bool linear, u8 texmap, u8* sample)
{
  const TexUnit& unit = bpmem.tex.GetUnit(texmap);
  const MipImage image = LocateMip(unit, mip, &s, &t);
  const int width = image.width_minus_1 + 1;
  const int height = image.height_minus_1 + 1;

  if (!linear)
  {
    const int ts = WrapTexel(s >> 7, image.wrap_s, width);
    const int tt = WrapTexel(t >> 7, image.wrap_t, height);
    image.Fetch(ts, tt, sample);
    return;
  }

  // Bilinear: texel centres are half a texel (64 in s17.7) from the integer coordinates.
  s -= 64;
  t -= 64;
  const u32 fs = static_cast<u32>(s & 0x7f);
  const u32 ft = static_cast<u32>(t & 0x7f);
  const int s0 = WrapTexel(s >> 7, image.wrap_s, width);
  const int t0 = WrapTexel(t >> 7, image.wrap_t, height);
  const int s1 = WrapTexel((s >> 7) + 1, image.wrap_s, width);
  const int t1 = WrapTexel((t >> 7) + 1, image.wrap_t, height);

  std::array<u32, 4> sum{};
  std::array<u8, 4> texel;
  image.Fetch(s0, t0, texel.data());
  Accumulate(sum, texel, (128 - fs) * (128 - ft));
  image.Fetch(s1, t0, texel.data());
  Accumulate(sum, texel, fs * (128 - ft));
  image.Fetch(s0, t1, texel.data());
  Accumulate(sum, texel, (128 - fs) * ft);
  image.Fetch(s1, t1, texel.data());
  Accumulate(sum, texel, fs * ft);

  // The weights add up to 128 * 128.
  for (size_t i = 0; i < 4; ++i)
    sample[i] = static_cast<u8>(sum[i] >> 14);
}
}  // namespace TextureSampler
