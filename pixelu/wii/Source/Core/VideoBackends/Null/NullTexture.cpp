// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Null/NullTexture.h"

#include <utility>

namespace Null
{
NullTexture::NullTexture(const TextureConfig& config) : AbstractTexture(config)
{
}

void NullTexture::CopyRectangleFromTexture(const AbstractTexture*, const MathUtil::Rectangle<int>&,
                                           u32, u32, const MathUtil::Rectangle<int>&, u32, u32)
{
}

void NullTexture::ResolveFromTexture(const AbstractTexture*, const MathUtil::Rectangle<int>&, u32,
                                     u32)
{
}

void NullTexture::Load(u32, u32, u32, u32, const u8*, size_t, u32)
{
}

NullStagingTexture::NullStagingTexture(StagingTextureType type, const TextureConfig& config)
    : AbstractStagingTexture(type, config)
{
  // Readers get a mapped buffer of the right size (zero-filled, since nothing writes to it).
  m_map_stride = m_texel_size * config.width;
  m_texture_buf.resize(m_map_stride * config.height);
  m_map_pointer = reinterpret_cast<char*>(m_texture_buf.data());
}

NullStagingTexture::~NullStagingTexture() = default;

void NullStagingTexture::CopyFromTexture(const AbstractTexture*, const MathUtil::Rectangle<int>&,
                                         u32, u32, const MathUtil::Rectangle<int>&)
{
  m_needs_flush = true;
}

void NullStagingTexture::CopyToTexture(const MathUtil::Rectangle<int>&, AbstractTexture*,
                                       const MathUtil::Rectangle<int>&, u32, u32)
{
  m_needs_flush = true;
}

void NullStagingTexture::Flush()
{
  m_needs_flush = false;
}

bool NullStagingTexture::Map()
{
  // Always mapped.
  return true;
}

void NullStagingTexture::Unmap()
{
}

std::unique_ptr<NullFramebuffer> NullFramebuffer::Create(NullTexture* color, NullTexture* depth,
                                                         std::vector<AbstractTexture*> extra_colors)
{
  if (!ValidateConfig(color, depth, extra_colors))
    return nullptr;

  // The colour attachment, when present, decides the framebuffer's dimensions.
  const NullTexture& sized_by = color ? *color : *depth;
  const auto color_fmt = color ? color->GetFormat() : AbstractTextureFormat::Undefined;
  const auto depth_fmt = depth ? depth->GetFormat() : AbstractTextureFormat::Undefined;
  return std::make_unique<NullFramebuffer>(color, depth, std::move(extra_colors), color_fmt,
                                           depth_fmt, sized_by.GetWidth(), sized_by.GetHeight(),
                                           sized_by.GetLayers(), sized_by.GetSamples());
}

NullFramebuffer::NullFramebuffer(AbstractTexture* color, AbstractTexture* depth,
                                 std::vector<AbstractTexture*> extra_colors,
                                 AbstractTextureFormat color_fmt, AbstractTextureFormat depth_fmt,
                                 u32 w, u32 h, u32 layer_count, u32 sample_count)
    : AbstractFramebuffer(color, depth, std::move(extra_colors), color_fmt, depth_fmt, w, h,
                          layer_count, sample_count)
{
}

}  // namespace Null
