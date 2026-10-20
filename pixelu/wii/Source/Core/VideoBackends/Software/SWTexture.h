// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"

namespace SW
{
// A texture held in system memory: one byte buffer per layer and mip level, each sized for four
// bytes per texel.
class SWTexture final : public AbstractTexture
{
public:
  explicit SWTexture(const TextureConfig& tex_config);
  ~SWTexture() override = default;

  void CopyRectangleFromTexture(const AbstractTexture* src,
                                const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                u32 dst_layer, u32 dst_level) override;
  void ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                          u32 layer, u32 level) override;
  void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer, size_t buffer_size,
            u32 layer) override;

  const u8* GetData(u32 layer, u32 level) const;
  u8* GetData(u32 layer, u32 level);

private:
  std::vector<std::vector<std::vector<u8>>> m_data;  // [layer][level]
};

// A staging texture is just a block of memory that is always mapped.
class SWStagingTexture final : public AbstractStagingTexture
{
public:
  explicit SWStagingTexture(StagingTextureType type, const TextureConfig& config);
  ~SWStagingTexture() override;

  void CopyFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& src_rect,
                       u32 src_layer, u32 src_level,
                       const MathUtil::Rectangle<int>& dst_rect) override;
  void CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                     u32 dst_level) override;

  bool Map() override;
  void Unmap() override;
  void Flush() override;

  // EFB copies write with the guest's row pitch rather than the texture's own.
  void SetMapStride(size_t stride) { m_map_stride = stride; }

private:
  std::vector<u8> m_data;
};

// Framebuffers only record their attachments; the software renderer draws to its EFB instead.
class SWFramebuffer final : public AbstractFramebuffer
{
public:
  explicit SWFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                         std::vector<AbstractTexture*> additional_color_attachments,
                         AbstractTextureFormat color_format, AbstractTextureFormat depth_format,
                         u32 width, u32 height, u32 layers, u32 samples);
  ~SWFramebuffer() override = default;

  static std::unique_ptr<SWFramebuffer>
  Create(SWTexture* color_attachment, SWTexture* depth_attachment,
         std::vector<AbstractTexture*> additional_color_attachments);
};

}  // namespace SW
