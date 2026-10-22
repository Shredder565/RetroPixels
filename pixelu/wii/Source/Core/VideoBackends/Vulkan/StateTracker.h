// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoCommon/Constants.h"

namespace Vulkan
{
class VKFramebuffer;
class VKShader;
class VKPipeline;
class VKTexture;
class StreamBuffer;
class VertexFormat;

// Mirrors the bindings the backend wants and records only what changed into the command
// buffer: vertex/index buffers, pipeline, viewport, scissor and the descriptor sets (new sets
// are allocated from the per-frame pools whenever their contents change). Also manages the
// render pass for the current framebuffer.
class StateTracker
{
public:
  StateTracker();
  ~StateTracker();

  static StateTracker* GetInstance();
  static bool CreateInstance();
  static void DestroyInstance();

  VKFramebuffer* GetFramebuffer() const { return m_framebuffer; }
  const VKPipeline* GetPipeline() const { return m_pipeline; }
  void SetVertexBuffer(VkBuffer buffer, VkDeviceSize offset, u32 size);
  void SetIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
  void SetFramebuffer(VKFramebuffer* framebuffer);
  void SetPipeline(const VKPipeline* pipeline);
  void SetComputeShader(const VKShader* shader);
  void SetGXUniformBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size);
  void SetUtilityUniformBuffer(VkBuffer buffer, u32 offset, u32 size);
  void SetTexture(u32 index, VkImageView view);
  void SetSampler(u32 index, VkSampler sampler);
  void SetSSBO(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
  void SetTexelBuffer(u32 index, VkBufferView view);
  void SetImageTexture(u32 index, VkImageView view);

  // Replaces every binding of this view with the dummy texture (the view is going away).
  void UnbindTexture(VkImageView view);

  // Forgets what was recorded, so the next Bind() records everything again (new command
  // buffer).
  void InvalidateCachedState();

  bool InRenderPass() const { return m_render_pass != VK_NULL_HANDLE; }
  // Begin a load (or don't-care) pass over the whole framebuffer; no-ops inside a pass.
  void BeginRenderPass();
  void BeginDiscardRenderPass();
  // Safe to call outside a pass. Bind() starts a new one when needed.
  void EndRenderPass();

  // Begins a clearing pass over `area`; must not be in a pass.
  void BeginClearRenderPass(const VkRect2D& area, const VkClearValue* clear_values,
                            u32 num_clear_values);
  // Ends the current pass only if it is the clearing pass.
  void EndClearRenderPass();

  void SetViewport(const VkViewport& viewport);
  void SetScissor(const VkRect2D& scissor);

  // Records the changed graphics state. False (draw nothing) without a pipeline.
  bool Bind();
  // Records the changed compute state. False (dispatch nothing) without a compute shader.
  bool BindCompute();

  // Whether the rectangle lies inside the current pass's render area.
  bool IsWithinRenderArea(s32 x, s32 y, u32 width, u32 height) const;

private:
  static constexpr u32 GX_SET_COUNT = 3;
  static constexpr u32 UTILITY_SET_COUNT = 2;

  // What has to be recorded (or rewritten) before the next draw or dispatch.
  enum Dirty : u32
  {
    GX_UBOS = 1u << 0,
    GX_UBO_OFFSETS = 1u << 1,
    UTILITY_UBO = 1u << 2,
    UTILITY_UBO_OFFSET = 1u << 3,
    GX_SAMPLERS = 1u << 4,
    GX_SSBO = 1u << 5,
    UTILITY_BINDINGS = 1u << 6,
    COMPUTE_BINDINGS = 1u << 7,
    VERTEX_BUFFER = 1u << 8,
    INDEX_BUFFER = 1u << 9,
    VIEWPORT = 1u << 10,
    SCISSOR = 1u << 11,
    PIPELINE = 1u << 12,
    COMPUTE_SHADER = 1u << 13,
    BIND_SETS = 1u << 14,
    BIND_COMPUTE_SET = 1u << 15,

    // Everything that needs a fresh descriptor set.
    ALL_SET_CONTENTS =
        GX_UBOS | UTILITY_UBO | GX_SAMPLERS | GX_SSBO | UTILITY_BINDINGS | COMPUTE_BINDINGS,
    // Texture or sampler changes touch every set that holds samplers.
    SAMPLER_USERS = GX_SAMPLERS | UTILITY_BINDINGS | COMPUTE_BINDINGS,
  };

  bool Initialize();
  void StartPass(VkRenderPass pass, const VkRect2D& area, u32 clear_count,
                 const VkClearValue* clears);
  bool ViewportInsideRenderArea() const;

  void WriteGXSets();
  void WriteUtilitySets();
  void WriteComputeSet();

  u32 m_dirty = 0;

  VkBuffer m_vertex_buffer = VK_NULL_HANDLE;
  VkDeviceSize m_vertex_offset = 0;
  VkBuffer m_index_buffer = VK_NULL_HANDLE;
  VkDeviceSize m_index_offset = 0;
  VkIndexType m_index_type = VK_INDEX_TYPE_UINT16;

  const VKPipeline* m_pipeline = nullptr;
  const VKShader* m_compute_shader = nullptr;

  // Descriptor contents. Sampled textures are shared by the GX, utility and compute sets.
  std::array<VkDescriptorBufferInfo, NUM_UBO_DESCRIPTOR_SET_BINDINGS> m_gx_ubos = {};
  std::array<u32, NUM_UBO_DESCRIPTOR_SET_BINDINGS> m_gx_ubo_offsets = {};
  VkDescriptorBufferInfo m_utility_ubo = {};
  u32 m_utility_ubo_offset = 0;
  std::array<VkDescriptorImageInfo, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_textures = {};
  std::array<VkBufferView, NUM_COMPUTE_TEXEL_BUFFERS> m_texel_buffers = {};
  VkDescriptorBufferInfo m_bbox_ssbo = {};
  VkDescriptorBufferInfo m_vertex_ssbo = {};
  std::array<VkDescriptorImageInfo, VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS> m_images = {};

  std::array<VkDescriptorSet, GX_SET_COUNT> m_gx_sets = {};
  std::array<VkDescriptorSet, UTILITY_SET_COUNT> m_utility_sets = {};
  VkDescriptorSet m_compute_set = VK_NULL_HANDLE;

  VkViewport m_viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
  VkRect2D m_scissor = {{0, 0}, {1, 1}};

  // Bound in place of missing textures and images.
  std::unique_ptr<VKTexture> m_null_texture;
  std::unique_ptr<VKTexture> m_null_image;

  VKFramebuffer* m_framebuffer = nullptr;
  VkRenderPass m_render_pass = VK_NULL_HANDLE;
  VkRect2D m_render_area = {};
};
}  // namespace Vulkan
