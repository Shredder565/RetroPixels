// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKTexture.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <tuple>
#include <utility>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/StagingBuffer.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKStreamBuffer.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
namespace
{
VkCommandBuffer DrawCmd()
{
  return g_command_buffer_mgr->GetCurrentCommandBuffer();
}

// The accesses and stages on one side of a layout barrier.
struct Scope
{
  VkAccessFlags access;
  VkPipelineStageFlags stages;
};

constexpr VkPipelineStageFlags DEPTH_TEST_STAGES =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
constexpr VkAccessFlags COLOR_RW =
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags DEPTH_RW =
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

// What earlier work on an image in this layout may have done, and must finish first.
Scope LeavingScope(VkImageLayout layout)
{
  switch (layout)
  {
  case VK_IMAGE_LAYOUT_PREINITIALIZED:
    return {VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT};
  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    return {COLOR_RW, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    return {DEPTH_RW, DEPTH_TEST_STAGES};
  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    return {VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    return {VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    return {VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
  default:
    // Undefined contents (or a layout we never leave from): nothing to wait for.
    return {0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT};
  }
}

// What the following work in this layout will do, and must wait.
Scope EnteringScope(VkImageLayout layout)
{
  switch (layout)
  {
  case VK_IMAGE_LAYOUT_UNDEFINED:
  case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
    return {0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT};
  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    return {COLOR_RW, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    return {DEPTH_RW, DEPTH_TEST_STAGES};
  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    return {VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    return {VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    return {VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
  default:
    return {0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
  }
}

// Compute shader accesses for a compute layout (Undefined: none).
VkAccessFlags ComputeAccess(VKTexture::ComputeImageLayout layout)
{
  switch (layout)
  {
  case VKTexture::ComputeImageLayout::ReadOnly:
    return VK_ACCESS_SHADER_READ_BIT;
  case VKTexture::ComputeImageLayout::WriteOnly:
    return VK_ACCESS_SHADER_WRITE_BIT;
  case VKTexture::ComputeImageLayout::ReadWrite:
    return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  default:
    return 0;
  }
}

VkImageMemoryBarrier LayoutBarrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                   const VkImageSubresourceRange& range)
{
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = range;
  return barrier;
}

void RecordBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                   const VkImageMemoryBarrier& barrier)
{
  vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

VkImageViewType ViewTypeFor(AbstractTextureType type, bool* known)
{
  *known = true;
  switch (type)
  {
  case AbstractTextureType::Texture_CubeMap:
    return VK_IMAGE_VIEW_TYPE_CUBE;
  case AbstractTextureType::Texture_2D:
    return VK_IMAGE_VIEW_TYPE_2D;
  case AbstractTextureType::Texture_2DArray:
    return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  default:
    *known = false;
    return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  }
}
}  // namespace

VKTexture::VKTexture(const TextureConfig& tex_config, VmaAllocation alloc, VkImage image,
                     std::string_view name, VkImageLayout layout, ComputeImageLayout compute_layout)
    : AbstractTexture(tex_config), m_alloc(alloc), m_image(image), m_layout(layout),
      m_compute_layout(compute_layout), m_name(name)
{
  if (m_name.empty() || !g_backend_info.bSupportsSettingObjectNames)
    return;

  VkDebugUtilsObjectNameInfoEXT label = {};
  label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  label.objectType = VK_OBJECT_TYPE_IMAGE;
  label.objectHandle = reinterpret_cast<uint64_t>(image);
  label.pObjectName = m_name.c_str();
  vkSetDebugUtilsObjectNameEXT(g_vulkan_context->GetDevice(), &label);
}

VKTexture::~VKTexture()
{
  StateTracker::GetInstance()->UnbindTexture(m_view);
  g_command_buffer_mgr->DeferImageViewDestruction(m_view);
  // Adopted images (swap chain) belong to someone else.
  if (m_alloc != VK_NULL_HANDLE)
    g_command_buffer_mgr->DeferImageDestruction(m_image, m_alloc);
}

std::unique_ptr<VKTexture> VKTexture::Create(const TextureConfig& tex_config, std::string_view name)
{
  const bool render_target = tex_config.IsRenderTarget();
  const bool compute_image = tex_config.IsComputeImage();

  VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (render_target)
  {
    usage |= IsDepthFormat(tex_config.format) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
                                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  }
  if (compute_image)
    usage |= VK_IMAGE_USAGE_STORAGE_BIT;

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  if (tex_config.type == AbstractTextureType::Texture_CubeMap)
    image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = GetVkFormatForHostTextureFormat(tex_config.format);
  image_info.extent = {tex_config.width, tex_config.height, 1};
  image_info.mipLevels = tex_config.levels;
  image_info.arrayLayers = tex_config.layers;
  image_info.samples = static_cast<VkSampleCountFlagBits>(tex_config.samples);
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = usage;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  // Render targets and compute images get the highest residency priority.
  VmaAllocationCreateInfo request = {};
  request.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  request.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  request.priority = (compute_image || render_target) ? 1.0f : 0.0f;

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  const VkResult res = vmaCreateImage(g_vulkan_context->GetMemoryAllocator(), &image_info,
                                      &request, &image, &alloc, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateImage failed: ");
    return nullptr;
  }

  auto texture = std::make_unique<VKTexture>(tex_config, alloc, image, name,
                                             VK_IMAGE_LAYOUT_UNDEFINED,
                                             ComputeImageLayout::Undefined);
  bool known_type = false;
  const VkImageViewType view_type = ViewTypeFor(tex_config.type, &known_type);
  if (!known_type)
  {
    PanicAlertFmt("Unhandled texture type.");
    return nullptr;
  }
  return texture->CreateView(view_type) ? std::move(texture) : nullptr;
}

std::unique_ptr<VKTexture> VKTexture::CreateAdopted(const TextureConfig& tex_config, VkImage image,
                                                    VkImageViewType view_type, VkImageLayout layout)
{
  auto texture = std::make_unique<VKTexture>(tex_config, VmaAllocation(VK_NULL_HANDLE), image, "",
                                             layout, ComputeImageLayout::Undefined);
  return texture->CreateView(view_type) ? std::move(texture) : nullptr;
}

VkImageSubresourceRange VKTexture::WholeImage() const
{
  return {GetImageAspectForFormat(GetFormat()), 0, GetLevels(), 0, GetLayers()};
}

bool VKTexture::CreateView(VkImageViewType type)
{
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = m_image;
  view_info.viewType = type;
  view_info.format = GetVkFormat();
  view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  view_info.subresourceRange = {GetImageViewAspectForFormat(GetFormat()), 0, GetLevels(), 0,
                                GetLayers()};

  const VkResult res =
      vkCreateImageView(g_vulkan_context->GetDevice(), &view_info, nullptr, &m_view);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateImageView failed: ");
    return false;
  }
  return true;
}

VkFormat VKTexture::GetLinearFormat(VkFormat format)
{
  // sRGB formats and their plain UNORM twins; anything else is already linear.
  static constexpr std::pair<VkFormat, VkFormat> SRGB_TO_UNORM[] = {
      {VK_FORMAT_R8_SRGB, VK_FORMAT_R8_UNORM},
      {VK_FORMAT_R8G8_SRGB, VK_FORMAT_R8G8_UNORM},
      {VK_FORMAT_R8G8B8_SRGB, VK_FORMAT_R8G8B8_UNORM},
      {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM},
      {VK_FORMAT_B8G8R8_SRGB, VK_FORMAT_B8G8R8_UNORM},
      {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM},
  };
  for (const auto& [srgb, unorm] : SRGB_TO_UNORM)
  {
    if (srgb == format)
      return unorm;
  }
  return format;
}

VkFormat VKTexture::GetVkFormatForHostTextureFormat(AbstractTextureFormat format)
{
  using F = AbstractTextureFormat;
  switch (format)
  {
  case F::DXT1:
    return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
  case F::DXT3:
    return VK_FORMAT_BC2_UNORM_BLOCK;
  case F::DXT5:
    return VK_FORMAT_BC3_UNORM_BLOCK;
  case F::BPTC:
    return VK_FORMAT_BC7_UNORM_BLOCK;
  case F::RGBA8:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case F::BGRA8:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case F::RGB10_A2:
    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  case F::RGBA16F:
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  case F::R16:
    return VK_FORMAT_R16_UNORM;
  case F::R32F:
    return VK_FORMAT_R32_SFLOAT;
  case F::D16:
    return VK_FORMAT_D16_UNORM;
  case F::D24_S8:
    return VK_FORMAT_D24_UNORM_S8_UINT;
  case F::D32F:
    return VK_FORMAT_D32_SFLOAT;
  case F::D32F_S8:
    return VK_FORMAT_D32_SFLOAT_S8_UINT;
  case F::Undefined:
    return VK_FORMAT_UNDEFINED;
  default:
    PanicAlertFmt("Unhandled texture format.");
    return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

VkImageAspectFlags VKTexture::GetImageAspectForFormat(AbstractTextureFormat format)
{
  if (format == AbstractTextureFormat::D24_S8 || format == AbstractTextureFormat::D32F_S8)
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  if (format == AbstractTextureFormat::D16 || format == AbstractTextureFormat::D32F)
    return VK_IMAGE_ASPECT_DEPTH_BIT;
  return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageAspectFlags VKTexture::GetImageViewAspectForFormat(AbstractTextureFormat format)
{
  // Views of combined depth/stencil images only see the depth.
  return (GetImageAspectForFormat(format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 ?
             VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT} :
             VkImageAspectFlags{VK_IMAGE_ASPECT_COLOR_BIT};
}

void VKTexture::CopyRectangleFromTexture(const AbstractTexture* src,
                                         const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                         u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                         u32 dst_layer, u32 dst_level)
{
  const auto* source = static_cast<const VKTexture*>(src);
  const u32 copy_w = static_cast<u32>(src_rect.GetWidth());
  const u32 copy_h = static_cast<u32>(src_rect.GetHeight());
  ASSERT_MSG(VIDEO, copy_w <= source->GetWidth() && copy_h <= source->GetHeight(),
             "Source rect is too large for CopyRectangleFromTexture");
  ASSERT_MSG(VIDEO,
             static_cast<u32>(dst_rect.GetWidth()) <= m_config.width &&
                 static_cast<u32>(dst_rect.GetHeight()) <= m_config.height,
             "Dest rect is too large for CopyRectangleFromTexture");

  VkImageCopy region = {};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src_level, src_layer, 1};
  region.srcOffset = {src_rect.left, src_rect.top, 0};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst_level, dst_layer, 1};
  region.dstOffset = {dst_rect.left, dst_rect.top, 0};
  region.extent = {copy_w, copy_h, 1};

  // Transfers cannot happen inside a render pass.
  StateTracker::GetInstance()->EndRenderPass();

  const VkImageLayout source_layout = source->GetLayout();
  source->TransitionToLayout(DrawCmd(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  TransitionToLayout(DrawCmd(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyImage(DrawCmd(), source->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  // The destination stays a transfer target until FinishedRendering().
  source->TransitionToLayout(DrawCmd(), source_layout);
}

void VKTexture::ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                                   u32 layer, u32 level)
{
  const auto* source = static_cast<const VKTexture*>(src);
  DEBUG_ASSERT(m_config.samples == 1 && m_config.width == source->m_config.width &&
               m_config.height == source->m_config.height && source->m_config.samples > 1);
  DEBUG_ASSERT(rect.left + rect.GetWidth() <= static_cast<int>(source->m_config.width) &&
               rect.top + rect.GetHeight() <= static_cast<int>(source->m_config.height));

  StateTracker::GetInstance()->EndRenderPass();
  const VkImageLayout source_layout = source->m_layout;
  source->TransitionToLayout(DrawCmd(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  TransitionToLayout(DrawCmd(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  VkImageResolve region = {};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
  region.srcOffset = {rect.left, rect.top, 0};
  region.dstSubresource = region.srcSubresource;
  region.dstOffset = region.srcOffset;
  region.extent = {static_cast<u32>(rect.GetWidth()), static_cast<u32>(rect.GetHeight()), 1};
  vkCmdResolveImage(DrawCmd(), source->m_image, source->m_layout, m_image, m_layout, 1, &region);

  source->TransitionToLayout(DrawCmd(), source_layout);
}

void VKTexture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                     size_t /*buffer_size*/, u32 layer)
{
  // Never copy past the level's extent.
  width = std::max(1u, std::min(width, GetWidth() >> level));
  height = std::max(1u, std::min(height, GetHeight() >> level));

  // The old contents do not matter, but an earlier submission may still sample the image, so a
  // real barrier is needed rather than an UNDEFINED override. The image stays a transfer target
  // until its last level and layer arrive. Uploads go into the init command buffer, which runs
  // before the draws recorded alongside it.
  TransitionToLayout(g_command_buffer_mgr->GetCurrentInitCommandBuffer(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  // Only the rows inside the texture are uploaded.
  const u32 alignment = static_cast<u32>(g_vulkan_context->GetBufferImageGranularity());
  const u32 block = GetBlockSizeForFormat(GetFormat());
  const u32 rows = Common::AlignUp(height, block) / block;
  const u32 pitch = CalculateStrideForFormat(m_config.format, row_length);
  const u32 upload_size = pitch * rows;

  std::unique_ptr<StagingBuffer> one_off;
  VkBuffer source_buffer = VK_NULL_HANDLE;
  VkDeviceSize source_offset = 0;
  if (upload_size <= STAGING_TEXTURE_UPLOAD_THRESHOLD)
  {
    StreamBuffer* stream = g_object_cache->GetTextureUploadBuffer();
    if (!stream->ReserveMemory(upload_size, alignment))
    {
      WARN_LOG_FMT(VIDEO, "Executing command list while waiting for space in texture upload buffer");
      VKGfx::GetInstance()->ExecuteCommandBuffer(false);
      // May wait for a fence this time.
      if (!stream->ReserveMemory(upload_size, alignment))
        PanicAlertFmt("Failed to allocate space in texture upload buffer");
    }
    source_buffer = stream->GetBuffer();
    source_offset = stream->GetCurrentOffset();
    std::memcpy(stream->GetCurrentHostPointer(), buffer, upload_size);
    stream->CommitMemory(upload_size);
  }
  else
  {
    // Too big for the ring: a buffer of its own, released once the copy has run.
    one_off = StagingBuffer::Create(STAGING_BUFFER_TYPE_UPLOAD, upload_size,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (one_off == nullptr || !one_off->Map())
    {
      PanicAlertFmt("Failed to allocate staging texture for large texture upload.");
      return;
    }
    source_buffer = one_off->GetBuffer();
    one_off->Write(0, buffer, upload_size, true);
    one_off->Unmap();
  }

  VkBufferImageCopy region = {};
  region.bufferOffset = source_offset;
  region.bufferRowLength = row_length;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
  region.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(g_command_buffer_mgr->GetCurrentInitCommandBuffer(), source_buffer,
                         m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // After the final piece the texture is most likely about to be sampled. Doing the transition
  // here keeps it in the init command buffer, out of any render pass.
  const bool last_piece = level == m_config.levels - 1 && layer == m_config.layers - 1;
  if (last_piece)
  {
    TransitionToLayout(g_command_buffer_mgr->GetCurrentInitCommandBuffer(),
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
}

void VKTexture::FinishedRendering()
{
  if (m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    return;
  StateTracker::GetInstance()->EndRenderPass();
  TransitionToLayout(DrawCmd(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VKTexture::OverrideImageLayout(VkImageLayout new_layout)
{
  m_layout = new_layout;
}

void VKTexture::TransitionToLayout(VkCommandBuffer command_buffer, VkImageLayout new_layout) const
{
  if (m_layout == new_layout)
    return;
  m_rendered_since_transition = false;

  Scope before = LeavingScope(m_layout);
  const Scope after = EnteringScope(new_layout);
  // Presenting waits for everything.
  if (new_layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    before.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  // An image last used by a compute shader must wait for that shader instead.
  if (m_compute_layout != ComputeImageLayout::Undefined)
  {
    before.access = ComputeAccess(m_compute_layout);
    before.stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  }
  m_compute_layout = ComputeImageLayout::Undefined;

  VkImageMemoryBarrier barrier = LayoutBarrier(m_image, m_layout, new_layout, WholeImage());
  barrier.srcAccessMask = before.access;
  barrier.dstAccessMask = after.access;
  RecordBarrier(command_buffer, before.stages, after.stages, barrier);
  m_layout = new_layout;
}

void VKTexture::TransitionToLayout(VkCommandBuffer command_buffer,
                                   ComputeImageLayout new_layout) const
{
  ASSERT(new_layout != ComputeImageLayout::Undefined);
  if (m_compute_layout == new_layout)
    return;
  m_rendered_since_transition = false;

  // Read-only compute access samples the image; anything that writes uses GENERAL.
  const Scope before = LeavingScope(m_layout);
  const VkImageLayout target = new_layout == ComputeImageLayout::ReadOnly ?
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL :
                                   VK_IMAGE_LAYOUT_GENERAL;
  const VkPipelineStageFlags after_stages =
      new_layout == ComputeImageLayout::Undefined ?
          VkPipelineStageFlags{0} :
          VkPipelineStageFlags{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};

  VkImageMemoryBarrier barrier = LayoutBarrier(m_image, m_layout, target, WholeImage());
  barrier.srcAccessMask = before.access;
  barrier.dstAccessMask = ComputeAccess(new_layout);
  m_layout = target;
  m_compute_layout = new_layout;
  RecordBarrier(command_buffer, before.stages, after_stages, barrier);
}

void VKTexture::PrepareForRenderPass(VkCommandBuffer command_buffer) const
{
  ASSERT(m_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
         m_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  if (!m_rendered_since_transition)
  {
    // The first pass after a transition is already ordered by that barrier.
    m_rendered_since_transition = true;
    return;
  }

  // A later pass must see (and not race) the previous pass's attachment writes.
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = m_layout;
  barrier.newLayout = m_layout;
  barrier.image = m_image;
  barrier.subresourceRange.aspectMask = GetImageAspectForFormat(GetFormat());
  barrier.subresourceRange.levelCount = GetLevels();
  barrier.subresourceRange.layerCount = GetLayers();

  VkPipelineStageFlags src_stage;
  VkPipelineStageFlags dst_stage;
  if ((barrier.subresourceRange.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0)
  {
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = COLOR_RW;
    src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  }
  else
  {
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = DEPTH_RW;
    src_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dst_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  }
  RecordBarrier(command_buffer, src_stage, dst_stage, barrier);
}

VKStagingTexture::VKStagingTexture(PrivateTag, StagingTextureType type, const TextureConfig& config,
                                   std::unique_ptr<StagingBuffer> buffer, VkImage linear_image,
                                   VmaAllocation linear_image_alloc)
    : AbstractStagingTexture(type, config), m_staging_buffer(std::move(buffer)),
      m_linear_image(linear_image), m_linear_image_alloc(linear_image_alloc)
{
}

VKStagingTexture::~VKStagingTexture()
{
  if (m_linear_image != VK_NULL_HANDLE)
    g_command_buffer_mgr->DeferImageDestruction(m_linear_image, m_linear_image_alloc);
}

std::unique_ptr<VKStagingTexture> VKStagingTexture::Create(StagingTextureType type,
                                                           const TextureConfig& config)
{
  const size_t stride = config.GetStride();
  const size_t size = stride * static_cast<size_t>(config.height);

  // Mutable textures go both ways but are placed like readback memory.
  STAGING_BUFFER_TYPE buffer_type = STAGING_BUFFER_TYPE_READBACK;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (type == StagingTextureType::Readback)
  {
    usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  else if (type == StagingTextureType::Upload)
  {
    buffer_type = STAGING_BUFFER_TYPE_UPLOAD;
    usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }

  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  char* mapped = nullptr;
  if (!StagingBuffer::AllocateBuffer(buffer_type, size, usage, &buffer, &alloc, &mapped))
    return nullptr;

  VkImage linear_image = VK_NULL_HANDLE;
  VmaAllocation linear_alloc = VK_NULL_HANDLE;
  const bool use_linear_image =
      DriverDetails::HasBug(DriverDetails::BUG_SLOW_OPTIMAL_IMAGE_TO_BUFFER_COPY) &&
      type == StagingTextureType::Readback && config.samples == 1;
  if (use_linear_image)
    std::tie(linear_image, linear_alloc) = CreateLinearImage(type, config);

  auto staging = std::make_unique<VKStagingTexture>(
      PrivateTag{}, type, config,
      std::make_unique<StagingBuffer>(buffer_type, buffer, alloc, size, mapped), linear_image,
      linear_alloc);
  if (!staging->m_staging_buffer->Map())
    return nullptr;
  staging->m_map_pointer = staging->m_staging_buffer->GetMapPointer();
  staging->m_map_stride = stride;
  return staging;
}

std::pair<VkImage, VmaAllocation> VKStagingTexture::CreateLinearImage(StagingTextureType type,
                                                                      const TextureConfig& config)
{
  static_cast<void>(type);
  const std::pair<VkImage, VmaAllocation> none{VK_NULL_HANDLE, VK_NULL_HANDLE};

  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VKTexture::GetVkFormatForHostTextureFormat(config.format);
  image_info.extent = {config.width, config.height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_LINEAR;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImageFormatProperties limits;
  VkResult res = vkGetPhysicalDeviceImageFormatProperties(
      g_vulkan_context->GetPhysicalDevice(), image_info.format, image_info.imageType,
      image_info.tiling, image_info.usage, image_info.flags, &limits);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "Linear images are not supported for the staging texture: ");
    return none;
  }

  VmaAllocationCreateInfo request = {};
  request.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  request.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  VkImage image = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  res = vmaCreateImage(g_vulkan_context->GetMemoryAllocator(), &image_info, &request, &image,
                       &alloc, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateImage failed: ");
    return none;
  }
  return {image, alloc};
}

VkDeviceSize VKStagingTexture::BufferOffsetOf(const MathUtil::Rectangle<int>& rect) const
{
  return static_cast<VkDeviceSize>(static_cast<size_t>(rect.top) * m_config.GetStride() +
                                   static_cast<size_t>(rect.left) * m_texel_size);
}

void VKStagingTexture::NoteCopyRecorded()
{
  m_needs_flush = true;
  m_copy_fence_counter = g_command_buffer_mgr->GetCurrentFenceCounter();
}

void VKStagingTexture::CopyFromTexture(const AbstractTexture* src,
                                       const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                       u32 src_level, const MathUtil::Rectangle<int>& dst_rect)
{
  const auto* source = static_cast<const VKTexture*>(src);
  ASSERT(m_type == StagingTextureType::Readback || m_type == StagingTextureType::Mutable);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());
  ASSERT(src_rect.left >= 0 && static_cast<u32>(src_rect.right) <= source->GetWidth() &&
         src_rect.top >= 0 && static_cast<u32>(src_rect.bottom) <= source->GetHeight());
  ASSERT(dst_rect.left >= 0 && static_cast<u32>(dst_rect.right) <= m_config.width &&
         dst_rect.top >= 0 && static_cast<u32>(dst_rect.bottom) <= m_config.height);

  StateTracker::GetInstance()->EndRenderPass();
  const VkImageLayout source_layout = source->GetLayout();
  source->TransitionToLayout(DrawCmd(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

  VkBufferImageCopy region = {};
  region.bufferOffset = BufferOffsetOf(dst_rect);
  region.bufferRowLength = m_config.width;
  // Subresource of the source texture; kept as-is when reading the (single-level) linear copy.
  region.imageSubresource = {VKTexture::GetImageViewAspectForFormat(source->GetFormat()),
                             src_level, src_layer, 1};
  region.imageOffset = {src_rect.left, src_rect.top, 0};
  region.imageExtent = {static_cast<u32>(src_rect.GetWidth()),
                        static_cast<u32>(src_rect.GetHeight()), 1u};

  VkImage copy_source = source->GetImage();
  if (m_linear_image != VK_NULL_HANDLE)
  {
    BlitToLinearImage(source, src_rect, src_layer, src_level, dst_rect);
    copy_source = m_linear_image;
    region.imageOffset = {0, 0, 0};
  }
  vkCmdCopyImageToBuffer(DrawCmd(), copy_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         m_staging_buffer->GetBuffer(), 1, &region);

  source->TransitionToLayout(DrawCmd(), source_layout);
  NoteCopyRecorded();
}

void VKStagingTexture::BlitToLinearImage(const VKTexture* src_tex,
                                         const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                         u32 src_level, const MathUtil::Rectangle<int>& dst_rect)
{
  // Some drivers (Qualcomm) allocate a temporary linear image inside every optimal-image to
  // buffer copy, which is slow. Blitting into our own reusable linear image avoids that.
  const VkImageAspectFlags aspect = VKTexture::GetImageViewAspectForFormat(src_tex->GetFormat());

  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.image = m_linear_image;
  barrier.subresourceRange = {aspect, 0, 1, 0, 1};
  RecordBarrier(DrawCmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, barrier);

  VkImageBlit blit;
  blit.srcSubresource = {aspect, src_level, src_layer, 1};
  blit.dstSubresource = {aspect, 0, 0, 1};
  blit.srcOffsets[0] = {src_rect.left, src_rect.top, 0};
  blit.srcOffsets[1] = {src_rect.left + src_rect.GetWidth(), src_rect.top + src_rect.GetHeight(),
                        1};
  blit.dstOffsets[0] = {0, 0, 0};
  blit.dstOffsets[1] = {dst_rect.GetWidth(), dst_rect.GetHeight(), 1};
  vkCmdBlitImage(DrawCmd(), src_tex->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 m_linear_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

  // Now make it the copy source.
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  RecordBarrier(DrawCmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, barrier);
}

void VKStagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                     u32 dst_level)
{
  const auto* target = static_cast<const VKTexture*>(dst);
  ASSERT(m_type == StagingTextureType::Upload || m_type == StagingTextureType::Mutable);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());
  ASSERT(src_rect.left >= 0 && static_cast<u32>(src_rect.right) <= m_config.width &&
         src_rect.top >= 0 && static_cast<u32>(src_rect.bottom) <= m_config.height);
  ASSERT(dst_rect.left >= 0 && static_cast<u32>(dst_rect.right) <= target->GetWidth() &&
         dst_rect.top >= 0 && static_cast<u32>(dst_rect.bottom) <= target->GetHeight());

  // CPU writes must reach memory before the GPU copy.
  m_staging_buffer->FlushCPUCache();
  StateTracker::GetInstance()->EndRenderPass();

  const VkImageLayout target_layout = target->GetLayout();
  target->TransitionToLayout(DrawCmd(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  VkBufferImageCopy region = {};
  region.bufferOffset = BufferOffsetOf(src_rect);
  region.bufferRowLength = m_config.width;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst_level, dst_layer, 1};
  region.imageOffset = {dst_rect.left, dst_rect.top, 0};
  region.imageExtent = {static_cast<u32>(dst_rect.GetWidth()),
                        static_cast<u32>(dst_rect.GetHeight()), 1u};
  vkCmdCopyBufferToImage(DrawCmd(), m_staging_buffer->GetBuffer(), target->GetImage(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  target->TransitionToLayout(DrawCmd(), target_layout);
  NoteCopyRecorded();
}

bool VKStagingTexture::Map()
{
  return true;
}

void VKStagingTexture::Unmap()
{
}

void VKStagingTexture::Flush()
{
  if (!m_needs_flush)
    return;

  if (m_copy_fence_counter == g_command_buffer_mgr->GetCurrentFenceCounter())
  {
    // The copy has not been submitted yet: submit and wait for it.
    VKGfx::GetInstance()->ExecuteCommandBuffer(false, true);
  }
  else
  {
    g_command_buffer_mgr->WaitForFenceCounter(m_copy_fence_counter);
  }

  if (m_type == StagingTextureType::Readback || m_type == StagingTextureType::Mutable)
    m_staging_buffer->InvalidateCPUCache();
  m_needs_flush = false;
}

namespace
{
AbstractTextureFormat FormatOrUndefined(const VKTexture* tex)
{
  return tex != nullptr ? tex->GetFormat() : AbstractTextureFormat::Undefined;
}
}  // namespace

VKFramebuffer::VKFramebuffer(VKTexture* color, VKTexture* depth,
                             std::vector<AbstractTexture*> extra_colors, u32 width, u32 height,
                             u32 layers, u32 samples, VkFramebuffer fb, VkRenderPass load_pass,
                             VkRenderPass discard_pass, VkRenderPass clear_pass)
    : AbstractFramebuffer(color, depth, std::move(extra_colors), FormatOrUndefined(color),
                          FormatOrUndefined(depth), width, height, layers, samples),
      m_fb(fb), m_load_pass(load_pass), m_discard_pass(discard_pass), m_clear_pass(clear_pass)
{
}

VKFramebuffer::~VKFramebuffer()
{
  g_command_buffer_mgr->DeferFramebufferDestruction(m_fb);
}

std::unique_ptr<VKFramebuffer>
VKFramebuffer::Create(VKTexture* color_attachment, VKTexture* depth_attachment,
                      std::vector<AbstractTexture*> additional_color_attachments)
{
  if (!ValidateConfig(color_attachment, depth_attachment, additional_color_attachments))
    return nullptr;

  const VkFormat color_format =
      color_attachment != nullptr ? color_attachment->GetVkFormat() : VK_FORMAT_UNDEFINED;
  const VkFormat depth_format =
      depth_attachment != nullptr ? depth_attachment->GetVkFormat() : VK_FORMAT_UNDEFINED;
  const VKTexture* reference = color_attachment != nullptr ? color_attachment : depth_attachment;
  const u32 width = reference->GetWidth();
  const u32 height = reference->GetHeight();
  const u32 layers = reference->GetLayers();
  const u32 samples = reference->GetSamples();

  // Attachment order: color, depth, then the extra colors (as in ObjectCache::GetRenderPass).
  std::vector<VkImageView> views;
  if (color_attachment != nullptr)
    views.push_back(color_attachment->GetView());
  if (depth_attachment != nullptr)
    views.push_back(depth_attachment->GetView());
  for (AbstractTexture* extra : additional_color_attachments)
    views.push_back(static_cast<VKTexture*>(extra)->GetView());

  const u8 extra_count = static_cast<u8>(additional_color_attachments.size());
  VkRenderPass passes[3] = {};
  const VkAttachmentLoadOp ops[3] = {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                     VK_ATTACHMENT_LOAD_OP_CLEAR};
  for (int n = 0; n < 3; ++n)
    passes[n] = g_object_cache->GetRenderPass(color_format, depth_format, samples, ops[n], extra_count);
  if (passes[0] == VK_NULL_HANDLE || passes[1] == VK_NULL_HANDLE || passes[2] == VK_NULL_HANDLE)
    return nullptr;

  VkFramebufferCreateInfo fb_info = {};
  fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_info.renderPass = passes[0];
  fb_info.attachmentCount = static_cast<u32>(views.size());
  fb_info.pAttachments = views.data();
  fb_info.width = width;
  fb_info.height = height;
  fb_info.layers = layers;

  VkFramebuffer fb = VK_NULL_HANDLE;
  const VkResult res = vkCreateFramebuffer(g_vulkan_context->GetDevice(), &fb_info, nullptr, &fb);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateFramebuffer failed: ");
    return nullptr;
  }

  return std::make_unique<VKFramebuffer>(color_attachment, depth_attachment,
                                         std::move(additional_color_attachments), width, height,
                                         layers, samples, fb, passes[0], passes[1], passes[2]);
}

void VKFramebuffer::Unbind()
{
  StateTracker* tracker = StateTracker::GetInstance();
  if (m_color_attachment != nullptr)
    tracker->UnbindTexture(static_cast<VKTexture*>(m_color_attachment)->GetView());
  for (AbstractTexture* extra : m_additional_color_attachments)
    tracker->UnbindTexture(static_cast<VKTexture*>(extra)->GetView());
  if (m_depth_attachment != nullptr)
    tracker->UnbindTexture(static_cast<VKTexture*>(m_depth_attachment)->GetView());
}

void VKFramebuffer::TransitionForRender()
{
  const VkCommandBuffer cmd = DrawCmd();
  if (m_color_attachment != nullptr)
  {
    static_cast<VKTexture*>(m_color_attachment)
        ->TransitionToLayout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  }
  for (AbstractTexture* extra : m_additional_color_attachments)
    static_cast<VKTexture*>(extra)->TransitionToLayout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  if (m_depth_attachment != nullptr)
  {
    static_cast<VKTexture*>(m_depth_attachment)
        ->TransitionToLayout(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  }
}

void VKFramebuffer::PrepareForRenderPass()
{
  const VkCommandBuffer cmd = DrawCmd();
  if (m_color_attachment != nullptr)
    static_cast<VKTexture*>(m_color_attachment)->PrepareForRenderPass(cmd);
  if (m_depth_attachment != nullptr)
    static_cast<VKTexture*>(m_depth_attachment)->PrepareForRenderPass(cmd);
  for (AbstractTexture* extra : m_additional_color_attachments)
    static_cast<VKTexture*>(extra)->PrepareForRenderPass(cmd);
}

void VKFramebuffer::SetAndClear(const VkRect2D& rect, const VkClearValue& color_value,
                                const VkClearValue& depth_value)
{
  // One clear value per attachment, in attachment order.
  // No reserve(): with no attachments data() stays null, which is what the pass is given.
  std::vector<VkClearValue> values;
  if (GetColorFormat() != AbstractTextureFormat::Undefined)
    values.push_back(color_value);
  if (GetDepthFormat() != AbstractTextureFormat::Undefined)
    values.push_back(depth_value);
  values.insert(values.end(), m_additional_color_attachments.size(), color_value);
  StateTracker::GetInstance()->BeginClearRenderPass(rect, values.data(),
                                                    static_cast<u32>(values.size()));
}
}  // namespace Vulkan
