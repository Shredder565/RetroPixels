// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"

namespace Vulkan
{
class StagingBuffer;

// A VkImage with one view over all of its levels and layers. The texture remembers the layout
// the image is in (as of the commands recorded so far) and records barriers when it changes.
class VKTexture final : public AbstractTexture
{
public:
  // Which compute shader accesses the image is set up for (in addition to its image layout).
  enum class ComputeImageLayout
  {
    Undefined,
    ReadOnly,
    WriteOnly,
    ReadWrite
  };

  VKTexture() = delete;
  VKTexture(const TextureConfig& tex_config, VmaAllocation alloc, VkImage image,
            std::string_view name, VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED,
            ComputeImageLayout compute_layout = ComputeImageLayout::Undefined);
  ~VKTexture() override;

  // The UNORM twin of an sRGB format (other formats are returned unchanged).
  static VkFormat GetLinearFormat(VkFormat format);
  static VkFormat GetVkFormatForHostTextureFormat(AbstractTextureFormat format);
  // Aspects for barriers (depth and stencil where present) and for views (depth only).
  static VkImageAspectFlags GetImageAspectForFormat(AbstractTextureFormat format);
  static VkImageAspectFlags GetImageViewAspectForFormat(AbstractTextureFormat format);

  void CopyRectangleFromTexture(const AbstractTexture* src,
                                const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                u32 dst_layer, u32 dst_level) override;
  void ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                          u32 layer, u32 level) override;
  void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer, size_t buffer_size,
            u32 layer) override;
  void FinishedRendering() override;

  VkImage GetImage() const { return m_image; }
  VkImageView GetView() const { return m_view; }
  VkImageLayout GetLayout() const { return m_layout; }
  VkFormat GetVkFormat() const { return GetVkFormatForHostTextureFormat(m_config.format); }
  // True when the image has its own memory (false for swap chain images).
  bool IsAdopted() const { return m_alloc != VmaAllocation(VK_NULL_HANDLE); }

  static std::unique_ptr<VKTexture> Create(const TextureConfig& tex_config, std::string_view name);
  // Wraps an image owned elsewhere; only the view is created (and destroyed) here.
  static std::unique_ptr<VKTexture>
  CreateAdopted(const TextureConfig& tex_config, VkImage image,
                VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED);

  // Records a layout change made outside this class (by a render pass), or marks the contents
  // as disposable with VK_IMAGE_LAYOUT_UNDEFINED.
  void OverrideImageLayout(VkImageLayout new_layout);

  void TransitionToLayout(VkCommandBuffer command_buffer, VkImageLayout new_layout) const;
  void TransitionToLayout(VkCommandBuffer command_buffer, ComputeImageLayout new_layout) const;
  // Called before each render pass that writes the image; adds a barrier between passes.
  void PrepareForRenderPass(VkCommandBuffer command_buffer) const;

private:
  bool CreateView(VkImageViewType type);
  VkImageSubresourceRange WholeImage() const;

  VmaAllocation m_alloc;
  VkImage m_image;
  VkImageView m_view = VK_NULL_HANDLE;
  // Layout tracking changes as commands are recorded, including through const references.
  mutable VkImageLayout m_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  mutable ComputeImageLayout m_compute_layout = ComputeImageLayout::Undefined;
  mutable bool m_rendered_since_transition = false;
  std::string m_name;
};

// A buffer the GPU copies texels into (readback) or out of (upload), always mapped.
class VKStagingTexture final : public AbstractStagingTexture
{
  struct PrivateTag
  {
  };

public:
  VKStagingTexture() = delete;
  VKStagingTexture(PrivateTag, StagingTextureType type, const TextureConfig& config,
                   std::unique_ptr<StagingBuffer> buffer, VkImage linear_image,
                   VmaAllocation linear_image_alloc);
  ~VKStagingTexture() override;

  void CopyFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& src_rect,
                       u32 src_layer, u32 src_level,
                       const MathUtil::Rectangle<int>& dst_rect) override;
  void CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                     u32 dst_level) override;

  bool Map() override;
  void Unmap() override;
  void Flush() override;

  static std::unique_ptr<VKStagingTexture> Create(StagingTextureType type,
                                                  const TextureConfig& config);

  // A linear-tiled single-level image of the config's size, or null handles if unsupported.
  static std::pair<VkImage, VmaAllocation> CreateLinearImage(StagingTextureType type,
                                                             const TextureConfig& config);

private:
  void BlitToLinearImage(const VKTexture* src_tex, const MathUtil::Rectangle<int>& src_rect,
                         u32 src_layer, u32 src_level, const MathUtil::Rectangle<int>& dst_rect);
  VkDeviceSize BufferOffsetOf(const MathUtil::Rectangle<int>& rect) const;
  void NoteCopyRecorded();

  std::unique_ptr<StagingBuffer> m_staging_buffer;
  // Only with BUG_SLOW_OPTIMAL_IMAGE_TO_BUFFER_COPY: readbacks go through this image.
  VkImage m_linear_image = VK_NULL_HANDLE;
  VmaAllocation m_linear_image_alloc = VK_NULL_HANDLE;
  // The submission holding the last copy.
  u64 m_copy_fence_counter = 0;
};

class VKFramebuffer final : public AbstractFramebuffer
{
public:
  VKFramebuffer(VKTexture* color_attachment, VKTexture* depth_attachment,
                std::vector<AbstractTexture*> additional_color_attachments, u32 width, u32 height,
                u32 layers, u32 samples, VkFramebuffer fb, VkRenderPass load_render_pass,
                VkRenderPass discard_render_pass, VkRenderPass clear_render_pass);
  ~VKFramebuffer() override;

  VkFramebuffer GetFB() const { return m_fb; }
  VkRect2D GetRect() const { return VkRect2D{{0, 0}, {m_width, m_height}}; }

  // The same attachments with load, don't-care and clear load operations.
  VkRenderPass GetLoadRenderPass() const { return m_load_pass; }
  VkRenderPass GetDiscardRenderPass() const { return m_discard_pass; }
  VkRenderPass GetClearRenderPass() const { return m_clear_pass; }

  // Removes the attachments from the texture bindings (they are about to be render targets).
  void Unbind();
  // Moves the attachments into their attachment layouts.
  void TransitionForRender();
  void PrepareForRenderPass();

  // Starts a clearing render pass over rect.
  void SetAndClear(const VkRect2D& rect, const VkClearValue& color_value,
                   const VkClearValue& depth_value);
  std::size_t GetNumberOfAdditonalAttachments() const
  {
    return m_additional_color_attachments.size();
  }

  static std::unique_ptr<VKFramebuffer>
  Create(VKTexture* color_attachments, VKTexture* depth_attachment,
         std::vector<AbstractTexture*> additional_color_attachments);

private:
  VkFramebuffer m_fb;
  VkRenderPass m_load_pass;
  VkRenderPass m_discard_pass;
  VkRenderPass m_clear_pass;
};

}  // namespace Vulkan
