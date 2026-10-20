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

bool NullStagingTexture::Map()
{
  return true;
}

void NullStagingTexture::Unmap()
{
}

void NullStagingTexture::Flush()
{
  m_needs_flush = false;
}

NullFramebuffer::NullFramebuffer(AbstractTexture* color_attachment,
                                 AbstractTexture* depth_attachment,
                                 std::vector<AbstractTexture*> additional_color_attachments,
                                 AbstractTextureFormat color_format,
                                 AbstractTextureFormat depth_format, u32 width, u32 height,
                                 u32 layers, u32 samples)
    : AbstractFramebuffer(color_attachment, depth_attachment,
                          std::move(additional_color_attachments), color_format, depth_format,
                          width, height, layers, samples)
{
}

std::unique_ptr<NullFramebuffer>
NullFramebuffer::Create(NullTexture* color_attachment, NullTexture* depth_attachment,
                        std::vector<AbstractTexture*> additional_color_attachments)
{
  if (!ValidateConfig(color_attachment, depth_attachment, additional_color_attachments))
    return nullptr;

  // The colour attachment, when present, decides the framebuffer's dimensions.
  const NullTexture& sized_by = color_attachment ? *color_attachment : *depth_attachment;
  const AbstractTextureFormat color_format =
      color_attachment ? color_attachment->GetFormat() : AbstractTextureFormat::Undefined;
  const AbstractTextureFormat depth_format =
      depth_attachment ? depth_attachment->GetFormat() : AbstractTextureFormat::Undefined;
  return std::make_unique<NullFramebuffer>(color_attachment, depth_attachment,
                                           std::move(additional_color_attachments), color_format,
                                           depth_format, sized_by.GetWidth(), sized_by.GetHeight(),
                                           sized_by.GetLayers(), sized_by.GetSamples());
}

}  // namespace Null
