// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/TextureConfig.h"

#include <algorithm>

#include "VideoCommon/AbstractTexture.h"

namespace
{
// One dimension of a mip level: halved per level, clamped to a single texel.
u32 MipExtent(u32 base_extent, u32 level)
{
  return std::max(base_extent >> level, 1u);
}
}  // namespace

bool TextureConfig::operator==(const TextureConfig& o) const
{
  const bool same_size = width == o.width && height == o.height;
  const bool same_shape = levels == o.levels && layers == o.layers && samples == o.samples;
  const bool same_kind = format == o.format && flags == o.flags && type == o.type;
  return same_size && same_shape && same_kind;
}

namespace
{
// A rectangle anchored at the origin.
MathUtil::Rectangle<int> OriginRect(u32 w, u32 h)
{
  return MathUtil::Rectangle<int>(0, 0, static_cast<int>(w), static_cast<int>(h));
}
}  // namespace

MathUtil::Rectangle<int> TextureConfig::GetRect() const
{
  // Unclamped: a zero-sized config gives an empty rectangle.
  return OriginRect(width, height);
}

MathUtil::Rectangle<int> TextureConfig::GetMipRect(u32 level) const
{
  return OriginRect(MipExtent(width, level), MipExtent(height, level));
}

size_t TextureConfig::GetStride() const
{
  return AbstractTexture::CalculateStrideForFormat(format, width);
}

size_t TextureConfig::GetMipStride(u32 level) const
{
  return AbstractTexture::CalculateStrideForFormat(format, MipExtent(width, level));
}
