// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/AbstractFramebuffer.h"

#include <algorithm>
#include <utility>

#include "VideoCommon/AbstractTexture.h"

AbstractFramebuffer::AbstractFramebuffer(AbstractTexture* color_attachment,
                                         AbstractTexture* depth_attachment,
                                         std::vector<AbstractTexture*> additional_color_attachments,
                                         AbstractTextureFormat color_format,
                                         AbstractTextureFormat depth_format, u32 width, u32 height,
                                         u32 layers, u32 samples)
    : m_color_attachment{color_attachment}, m_depth_attachment{depth_attachment},
      m_additional_color_attachments{std::move(additional_color_attachments)},
      m_color_format{color_format}, m_depth_format{depth_format}, m_width{width},
      m_height{height}, m_layers{layers}, m_samples{samples}
{
}

AbstractFramebuffer::~AbstractFramebuffer() = default;

namespace
{
// Attachments have to be render targets with exactly one mip level.
bool IsUsableAttachment(const AbstractTexture* texture)
{
  const TextureConfig& cfg = texture->GetConfig();
  return cfg.IsRenderTarget() && cfg.levels == 1;
}

bool SameSizeAndSamples(const TextureConfig& a, const TextureConfig& b)
{
  return a.width == b.width && a.height == b.height && a.samples == b.samples;
}
}  // namespace

bool AbstractFramebuffer::ValidateConfig(
    const AbstractTexture* color_attachment, const AbstractTexture* depth_attachment,
    const std::vector<AbstractTexture*>& additional_color_attachments)
{
  if (color_attachment == nullptr)
  {
    // Depth-only targets are allowed, but extra colour targets need a main colour target.
    if (depth_attachment == nullptr || !additional_color_attachments.empty())
      return false;
    return IsUsableAttachment(depth_attachment);
  }

  if (!IsUsableAttachment(color_attachment))
    return false;
  if (depth_attachment != nullptr && !IsUsableAttachment(depth_attachment))
    return false;
  if (!std::ranges::all_of(additional_color_attachments, IsUsableAttachment))
    return false;

  const TextureConfig& main_cfg = color_attachment->GetConfig();
  if (depth_attachment != nullptr)
  {
    const TextureConfig& depth_cfg = depth_attachment->GetConfig();
    if (!SameSizeAndSamples(main_cfg, depth_cfg) || main_cfg.layers != depth_cfg.layers)
      return false;
  }

  // Extra colour targets may differ from the main one in layer count only.
  return std::ranges::all_of(additional_color_attachments, [&](const AbstractTexture* extra) {
    return SameSizeAndSamples(main_cfg, extra->GetConfig());
  });
}

MathUtil::Rectangle<int> AbstractFramebuffer::GetRect() const
{
  return {0, 0, static_cast<int>(m_width), static_cast<int>(m_height)};
}
