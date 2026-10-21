// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/StateTracker.h"

#include <cstring>

#include "Common/Assert.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKPipeline.h"
#include "VideoBackends/Vulkan/VKShader.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/Constants.h"

namespace Vulkan
{
namespace
{
std::unique_ptr<StateTracker> s_tracker;

VkCommandBuffer Cmd()
{
  return g_command_buffer_mgr->GetCurrentCommandBuffer();
}

VkDescriptorSet NewSet(DESCRIPTOR_SET_LAYOUT layout)
{
  return g_command_buffer_mgr->AllocateDescriptorSet(g_object_cache->GetDescriptorSetLayout(layout));
}

VkWriteDescriptorSet BufferWrite(VkDescriptorSet set, u32 binding, VkDescriptorType type,
                                 const VkDescriptorBufferInfo* info)
{
  return {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, 1, type, nullptr,
          info, nullptr};
}

VkWriteDescriptorSet ImageWrite(VkDescriptorSet set, u32 binding, u32 count,
                                VkDescriptorType type, const VkDescriptorImageInfo* images)
{
  return {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, count, type, images,
          nullptr, nullptr};
}

VkWriteDescriptorSet TexelWrite(VkDescriptorSet set, u32 binding, u32 count,
                                const VkBufferView* views)
{
  return {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          nullptr,
          set,
          binding,
          0,
          count,
          VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
          nullptr,
          nullptr,
          views};
}

bool SameBuffer(const VkDescriptorBufferInfo& info, VkBuffer buffer, VkDeviceSize offset,
                VkDeviceSize range)
{
  return info.buffer == buffer && info.offset == offset && info.range == range;
}
}  // namespace

StateTracker::StateTracker() = default;
StateTracker::~StateTracker() = default;

StateTracker* StateTracker::GetInstance()
{
  return s_tracker.get();
}

bool StateTracker::CreateInstance()
{
  ASSERT(!s_tracker);
  s_tracker = std::make_unique<StateTracker>();
  if (s_tracker->Initialize())
    return true;
  s_tracker.reset();
  return false;
}

void StateTracker::DestroyInstance()
{
  if (!s_tracker)
    return;

  // A dying texture unbinds itself through this tracker. Clear the bindings first so the
  // dummies never find (and rebind) themselves while being destroyed.
  for (VkDescriptorImageInfo& texture : s_tracker->m_textures)
    texture.imageView = VK_NULL_HANDLE;
  for (VkDescriptorImageInfo& image : s_tracker->m_images)
    image.imageView = VK_NULL_HANDLE;
  s_tracker->m_null_texture.reset();
  s_tracker->m_null_image.reset();
  s_tracker.reset();
}

bool StateTracker::Initialize()
{
  const VkCommandBuffer init_cmd = g_command_buffer_mgr->GetCurrentInitCommandBuffer();

  m_null_texture = VKTexture::Create(TextureConfig(1, 1, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                                                   AbstractTextureType::Texture_2DArray),
                                     "");
  if (!m_null_texture)
    return false;
  m_null_texture->TransitionToLayout(init_cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  m_null_image = VKTexture::Create(TextureConfig(1, 1, 1, 1, 1, AbstractTextureFormat::RGBA8,
                                                 AbstractTextureFlag_ComputeImage,
                                                 AbstractTextureType::Texture_2DArray),
                                   "");
  if (!m_null_image)
    return false;
  m_null_image->TransitionToLayout(init_cmd, VK_IMAGE_LAYOUT_GENERAL);

  const VkSampler point = g_object_cache->GetPointSampler();
  for (VkDescriptorImageInfo& texture : m_textures)
    texture = {point, m_null_texture->GetView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  for (VkDescriptorImageInfo& image : m_images)
    image = {point, m_null_image->GetView(), VK_IMAGE_LAYOUT_GENERAL};

  InvalidateCachedState();
  return true;
}

void StateTracker::SetVertexBuffer(VkBuffer buffer, VkDeviceSize offset, u32 size)
{
  if (m_vertex_buffer != buffer || m_vertex_offset != offset)
  {
    m_vertex_buffer = buffer;
    m_vertex_offset = offset;
    m_dirty |= VERTEX_BUFFER;
  }
  // The same vertices, read as storage by the dynamic vertex loader.
  if (!SameBuffer(m_vertex_ssbo, buffer, offset, size))
  {
    m_vertex_ssbo = {buffer, offset, size};
    m_dirty |= GX_SSBO;
  }
}

void StateTracker::SetIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
  if (m_index_buffer == buffer && m_index_offset == offset && m_index_type == type)
    return;
  m_index_buffer = buffer;
  m_index_offset = offset;
  m_index_type = type;
  m_dirty |= INDEX_BUFFER;
}

void StateTracker::SetFramebuffer(VKFramebuffer* framebuffer)
{
  ASSERT(!InRenderPass());
  m_framebuffer = framebuffer;
}

void StateTracker::SetPipeline(const VKPipeline* pipeline)
{
  if (pipeline == m_pipeline)
    return;

  // A different usage means a different pipeline layout: every set must be bound again.
  const bool layout_changes =
      pipeline != nullptr && (m_pipeline == nullptr || pipeline->GetUsage() != m_pipeline->GetUsage());
  m_pipeline = pipeline;
  m_dirty |= PIPELINE;
  if (layout_changes)
    m_dirty |= BIND_SETS;
}

void StateTracker::SetComputeShader(const VKShader* shader)
{
  if (shader == m_compute_shader)
    return;
  m_compute_shader = shader;
  m_dirty |= COMPUTE_SHADER;
}

void StateTracker::SetGXUniformBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size)
{
  // Buffers are dynamic: only a new buffer or size needs a new set, a new offset is a rebind.
  VkDescriptorBufferInfo& ubo = m_gx_ubos[index];
  if (ubo.buffer != buffer || ubo.range != size)
  {
    ubo.buffer = buffer;
    ubo.range = size;
    m_dirty |= GX_UBOS;
  }
  if (m_gx_ubo_offsets[index] != offset)
  {
    m_gx_ubo_offsets[index] = offset;
    m_dirty |= GX_UBO_OFFSETS;
  }
}

void StateTracker::SetUtilityUniformBuffer(VkBuffer buffer, u32 offset, u32 size)
{
  // Shared by utility draws and compute dispatches.
  if (m_utility_ubo.buffer != buffer || m_utility_ubo.range != size)
  {
    m_utility_ubo.buffer = buffer;
    m_utility_ubo.range = size;
    m_dirty |= UTILITY_UBO;
  }
  if (m_utility_ubo_offset != offset)
  {
    m_utility_ubo_offset = offset;
    m_dirty |= UTILITY_UBO_OFFSET | BIND_COMPUTE_SET;
  }
}

void StateTracker::SetTexture(u32 index, VkImageView view)
{
  VkDescriptorImageInfo& texture = m_textures[index];
  if (texture.imageView == view)
    return;
  texture.imageView = view;
  texture.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  m_dirty |= SAMPLER_USERS;
}

void StateTracker::SetSampler(u32 index, VkSampler sampler)
{
  VkDescriptorImageInfo& texture = m_textures[index];
  if (texture.sampler == sampler)
    return;
  texture.sampler = sampler;
  m_dirty |= SAMPLER_USERS;
}

void StateTracker::SetSSBO(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range)
{
  if (SameBuffer(m_bbox_ssbo, buffer, offset, range))
    return;
  m_bbox_ssbo = {buffer, offset, range};
  m_dirty |= GX_SSBO;
}

void StateTracker::SetTexelBuffer(u32 index, VkBufferView view)
{
  if (m_texel_buffers[index] == view)
    return;
  m_texel_buffers[index] = view;
  m_dirty |= UTILITY_BINDINGS | COMPUTE_BINDINGS;
}

void StateTracker::SetImageTexture(u32 index, VkImageView view)
{
  VkDescriptorImageInfo& image = m_images[index];
  if (image.imageView == view)
    return;
  image.imageView = view;
  image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  m_dirty |= COMPUTE_BINDINGS;
}

void StateTracker::UnbindTexture(VkImageView view)
{
  // Only the stored bindings change; sets already written keep the old view until rewritten.
  for (VkDescriptorImageInfo& texture : m_textures)
  {
    if (texture.imageView != view)
      continue;
    texture.imageView = m_null_texture->GetView();
    texture.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  for (VkDescriptorImageInfo& image : m_images)
  {
    if (image.imageView != view)
      continue;
    image.imageView = m_null_image->GetView();
    // Recorded as a sampled layout, as it always has been.
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
}

void StateTracker::InvalidateCachedState()
{
  m_gx_sets.fill(VK_NULL_HANDLE);
  m_utility_sets.fill(VK_NULL_HANDLE);
  m_compute_set = VK_NULL_HANDLE;
  m_dirty |= ALL_SET_CONTENTS | VIEWPORT | SCISSOR | PIPELINE | COMPUTE_SHADER | BIND_SETS |
             BIND_COMPUTE_SET;
  if (m_vertex_buffer != VK_NULL_HANDLE)
    m_dirty |= VERTEX_BUFFER;
  if (m_index_buffer != VK_NULL_HANDLE)
    m_dirty |= INDEX_BUFFER;
}

void StateTracker::StartPass(VkRenderPass pass, const VkRect2D& area, u32 clear_count,
                             const VkClearValue* clears)
{
  m_render_pass = pass;
  m_render_area = area;
  const VkRenderPassBeginInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                      nullptr,
                                      m_render_pass,
                                      m_framebuffer->GetFB(),
                                      m_render_area,
                                      clear_count,
                                      clears};
  vkCmdBeginRenderPass(Cmd(), &info, VK_SUBPASS_CONTENTS_INLINE);
}

void StateTracker::BeginRenderPass()
{
  if (InRenderPass())
    return;
  m_render_pass = m_framebuffer->GetLoadRenderPass();
  m_render_area = m_framebuffer->GetRect();
  m_framebuffer->PrepareForRenderPass();
  StartPass(m_render_pass, m_render_area, 0, nullptr);
}

void StateTracker::BeginDiscardRenderPass()
{
  if (InRenderPass())
    return;
  // The contents are about to be overwritten: no barrier against the previous pass.
  StartPass(m_framebuffer->GetDiscardRenderPass(), m_framebuffer->GetRect(), 0, nullptr);
}

void StateTracker::EndRenderPass()
{
  if (!InRenderPass())
    return;
  vkCmdEndRenderPass(Cmd());
  m_render_pass = VK_NULL_HANDLE;
}

void StateTracker::BeginClearRenderPass(const VkRect2D& area, const VkClearValue* clear_values,
                                        u32 num_clear_values)
{
  ASSERT(!InRenderPass());
  m_render_pass = m_framebuffer->GetClearRenderPass();
  m_render_area = area;
  m_framebuffer->PrepareForRenderPass();
  StartPass(m_render_pass, m_render_area, num_clear_values, clear_values);
}

void StateTracker::EndClearRenderPass()
{
  // Bind() will then start the load pass.
  if (m_render_pass == m_framebuffer->GetClearRenderPass())
    EndRenderPass();
}

void StateTracker::SetViewport(const VkViewport& viewport)
{
  // Bitwise, so that -0.0 and NaN changes are recorded too.
  if (std::memcmp(&m_viewport, &viewport, sizeof(viewport)) == 0)
    return;
  m_viewport = viewport;
  m_dirty |= VIEWPORT;
}

void StateTracker::SetScissor(const VkRect2D& scissor)
{
  if (std::memcmp(&m_scissor, &scissor, sizeof(scissor)) == 0)
    return;
  m_scissor = scissor;
  m_dirty |= SCISSOR;
}

bool StateTracker::IsWithinRenderArea(s32 x, s32 y, u32 width, u32 height) const
{
  const s32 area_x0 = m_render_area.offset.x;
  const s32 area_y0 = m_render_area.offset.y;
  const s32 area_x1 = area_x0 + static_cast<s32>(m_render_area.extent.width);
  const s32 area_y1 = area_y0 + static_cast<s32>(m_render_area.extent.height);
  const s32 x1 = x + static_cast<s32>(width);
  const s32 y1 = y + static_cast<s32>(height);
  return x >= area_x0 && x1 <= area_x1 && y >= area_y0 && y1 <= area_y1;
}

bool StateTracker::ViewportInsideRenderArea() const
{
  return IsWithinRenderArea(static_cast<s32>(m_viewport.x), static_cast<s32>(m_viewport.y),
                            static_cast<u32>(m_viewport.width),
                            static_cast<u32>(m_viewport.height));
}

bool StateTracker::Bind()
{
  if (m_pipeline == nullptr)
    return false;

  // A clearing pass only covers its clear rectangle; drawing outside it needs the full pass.
  if (m_render_pass == m_framebuffer->GetClearRenderPass() && !ViewportInsideRenderArea())
    EndRenderPass();

  if (m_pipeline->GetUsage() == AbstractPipelineUsage::Utility)
    WriteUtilitySets();
  else
    WriteGXSets();

  if (!InRenderPass())
    BeginRenderPass();

  const VkCommandBuffer cmd = Cmd();
  const u32 dirty = m_dirty;

  // Ubershaders with the dynamic vertex loader fetch vertices from the storage buffer. The
  // vertex buffer stays pending for the next pipeline that uses it.
  const bool uses_vertex_buffer = !g_backend_info.bSupportsDynamicVertexLoader ||
                                  m_pipeline->GetUsage() != AbstractPipelineUsage::GXUber;
  u32 recorded = INDEX_BUFFER | PIPELINE | VIEWPORT | SCISSOR;
  if (uses_vertex_buffer && (dirty & VERTEX_BUFFER) != 0)
  {
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertex_buffer, &m_vertex_offset);
    recorded |= VERTEX_BUFFER;
  }
  if ((dirty & INDEX_BUFFER) != 0)
    vkCmdBindIndexBuffer(cmd, m_index_buffer, m_index_offset, m_index_type);
  if ((dirty & PIPELINE) != 0)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline->GetVkPipeline());
  if ((dirty & VIEWPORT) != 0)
    vkCmdSetViewport(cmd, 0, 1, &m_viewport);
  if ((dirty & SCISSOR) != 0)
    vkCmdSetScissor(cmd, 0, 1, &m_scissor);

  m_dirty &= ~recorded;
  return true;
}

bool StateTracker::BindCompute()
{
  if (m_compute_shader == nullptr)
    return false;

  // Dispatches cannot happen inside a render pass.
  EndRenderPass();

  if ((m_dirty & COMPUTE_SHADER) != 0)
  {
    vkCmdBindPipeline(Cmd(), VK_PIPELINE_BIND_POINT_COMPUTE,
                      m_compute_shader->GetComputePipeline());
  }
  WriteComputeSet();
  m_dirty &= ~static_cast<u32>(COMPUTE_SHADER);
  return true;
}

void StateTracker::WriteGXSets()
{
  // Set 0: GX uniform buffers. Set 1: pixel samplers. Set 2: storage buffers.
  std::array<VkWriteDescriptorSet, NUM_UBO_DESCRIPTOR_SET_BINDINGS + 3> writes;
  u32 count = 0;

  const bool gs_ubo_used =
      g_backend_info.bSupportsGeometryShaders || g_ActiveConfig.UseVSForLinePointExpand();
  const u32 dynamic_offsets = gs_ubo_used ? NUM_UBO_DESCRIPTOR_SET_BINDINGS :
                                            NUM_UBO_DESCRIPTOR_SET_BINDINGS - 1;

  if ((m_dirty & GX_UBOS) != 0 || m_gx_sets[0] == VK_NULL_HANDLE)
  {
    m_gx_sets[0] = NewSet(DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS);
    for (u32 binding = 0; binding < NUM_UBO_DESCRIPTOR_SET_BINDINGS; ++binding)
    {
      if (binding == UBO_DESCRIPTOR_SET_BINDING_GS && !gs_ubo_used)
        continue;
      // No custom shader has run yet: its buffer is still empty.
      if (binding == UBO_DESCRIPTOR_SET_BINDING_CUST && m_gx_ubos[binding].range == 0)
        continue;
      writes[count++] = BufferWrite(m_gx_sets[0], binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                    &m_gx_ubos[binding]);
    }
    m_dirty = (m_dirty & ~static_cast<u32>(GX_UBOS)) | BIND_SETS;
  }

  if ((m_dirty & GX_SAMPLERS) != 0 || m_gx_sets[1] == VK_NULL_HANDLE)
  {
    m_gx_sets[1] = NewSet(DESCRIPTOR_SET_LAYOUT_STANDARD_SAMPLERS);
    writes[count++] =
        ImageWrite(m_gx_sets[1], 0, static_cast<u32>(VideoCommon::MAX_PIXEL_SHADER_SAMPLERS),
                   VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_textures.data());
    m_dirty = (m_dirty & ~static_cast<u32>(GX_SAMPLERS)) | BIND_SETS;
  }

  const bool vs_expand = g_ActiveConfig.UseVSForLinePointExpand();
  const bool uber_vertices = g_backend_info.bSupportsDynamicVertexLoader &&
                             m_pipeline->GetUsage() == AbstractPipelineUsage::GXUber;
  const bool storage_set_used = g_backend_info.bSupportsBBox || uber_vertices || vs_expand;
  if (storage_set_used && ((m_dirty & GX_SSBO) != 0 || m_gx_sets[2] == VK_NULL_HANDLE))
  {
    m_gx_sets[2] = NewSet(DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS);
    writes[count++] =
        BufferWrite(m_gx_sets[2], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &m_bbox_ssbo);
    // The layout only has the vertex binding when something can read it.
    if (g_backend_info.bSupportsDynamicVertexLoader || vs_expand)
    {
      writes[count++] =
          BufferWrite(m_gx_sets[2], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &m_vertex_ssbo);
    }
    m_dirty = (m_dirty & ~static_cast<u32>(GX_SSBO)) | BIND_SETS;
  }

  if (count != 0)
    vkUpdateDescriptorSets(g_vulkan_context->GetDevice(), count, writes.data(), 0, nullptr);

  if ((m_dirty & BIND_SETS) != 0)
  {
    vkCmdBindDescriptorSets(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipeline->GetVkPipelineLayout(), 0,
                            storage_set_used ? GX_SET_COUNT : GX_SET_COUNT - 1, m_gx_sets.data(),
                            dynamic_offsets, m_gx_ubo_offsets.data());
    m_dirty &= ~static_cast<u32>(BIND_SETS | GX_UBO_OFFSETS);
  }
  else if ((m_dirty & GX_UBO_OFFSETS) != 0)
  {
    // Only the offsets moved: rebind the uniform set alone.
    vkCmdBindDescriptorSets(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipeline->GetVkPipelineLayout(), 0, 1, m_gx_sets.data(),
                            dynamic_offsets, m_gx_ubo_offsets.data());
    m_dirty &= ~static_cast<u32>(GX_UBO_OFFSETS);
  }
}

void StateTracker::WriteUtilitySets()
{
  // Set 0: the utility uniform buffer. Set 1: samplers and one texel buffer.
  std::array<VkWriteDescriptorSet, 3> writes;
  u32 count = 0;

  if ((m_dirty & UTILITY_UBO) != 0 || m_utility_sets[0] == VK_NULL_HANDLE)
  {
    m_utility_sets[0] = NewSet(DESCRIPTOR_SET_LAYOUT_UTILITY_UNIFORM_BUFFER);
    writes[count++] = BufferWrite(m_utility_sets[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                  &m_utility_ubo);
    m_dirty = (m_dirty & ~static_cast<u32>(UTILITY_UBO)) | BIND_SETS;
  }

  if ((m_dirty & UTILITY_BINDINGS) != 0 || m_utility_sets[1] == VK_NULL_HANDLE)
  {
    m_utility_sets[1] = NewSet(DESCRIPTOR_SET_LAYOUT_UTILITY_SAMPLERS);
    writes[count++] = ImageWrite(m_utility_sets[1], 0, NUM_UTILITY_PIXEL_SAMPLERS,
                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_textures.data());
    writes[count++] = TexelWrite(m_utility_sets[1], 8, 1, m_texel_buffers.data());
    m_dirty = (m_dirty & ~static_cast<u32>(UTILITY_BINDINGS)) | BIND_SETS;
  }

  if (count != 0)
    vkUpdateDescriptorSets(g_vulkan_context->GetDevice(), count, writes.data(), 0, nullptr);

  // Either both sets, or only the uniform set for a new offset.
  const u32 sets_to_bind =
      (m_dirty & BIND_SETS) != 0 ? UTILITY_SET_COUNT : ((m_dirty & UTILITY_UBO_OFFSET) != 0 ? 1 : 0);
  if (sets_to_bind == 0)
    return;
  vkCmdBindDescriptorSets(Cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                          m_pipeline->GetVkPipelineLayout(), 0, sets_to_bind,
                          m_utility_sets.data(), 1, &m_utility_ubo_offset);
  m_dirty &= ~static_cast<u32>(BIND_SETS | UTILITY_UBO_OFFSET);
}

void StateTracker::WriteComputeSet()
{
  // One set: uniform buffer, samplers, texel buffers, storage images (see the set layout).
  if ((m_dirty & COMPUTE_BINDINGS) != 0)
  {
    constexpr u32 SAMPLERS = VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS;
    m_compute_set = NewSet(DESCRIPTOR_SET_LAYOUT_COMPUTE);
    const std::array<VkWriteDescriptorSet, 4> writes = {
        BufferWrite(m_compute_set, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &m_utility_ubo),
        ImageWrite(m_compute_set, 1, SAMPLERS, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   m_textures.data()),
        TexelWrite(m_compute_set, 1 + SAMPLERS, NUM_COMPUTE_TEXEL_BUFFERS, m_texel_buffers.data()),
        ImageWrite(m_compute_set, 1 + SAMPLERS + NUM_COMPUTE_TEXEL_BUFFERS, SAMPLERS,
                   VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, m_images.data()),
    };
    vkUpdateDescriptorSets(g_vulkan_context->GetDevice(), static_cast<u32>(writes.size()),
                           writes.data(), 0, nullptr);
    m_dirty = (m_dirty & ~static_cast<u32>(COMPUTE_BINDINGS)) | BIND_COMPUTE_SET;
  }

  if ((m_dirty & BIND_COMPUTE_SET) != 0)
  {
    vkCmdBindDescriptorSets(Cmd(), VK_PIPELINE_BIND_POINT_COMPUTE,
                            g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_COMPUTE), 0, 1,
                            &m_compute_set, 1, &m_utility_ubo_offset);
    m_dirty &= ~static_cast<u32>(BIND_COMPUTE_SET);
  }
}

}  // namespace Vulkan
