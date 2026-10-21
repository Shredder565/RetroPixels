// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <map>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"

#include "VideoBackends/Vulkan/Constants.h"

#include "VideoCommon/RenderState.h"

namespace Vulkan
{
class StreamBuffer;

// Long-lived objects shared by the whole backend: descriptor set and pipeline layouts, the
// fixed and per-state samplers, render passes, the texture upload stream buffer and the
// VkPipelineCache (kept on disk between runs).
class ObjectCache
{
public:
  ObjectCache();
  ~ObjectCache();

  bool Initialize();
  // Writes the pipeline cache to disk (when shader caching is on).
  void Shutdown();

  VkDescriptorSetLayout GetDescriptorSetLayout(DESCRIPTOR_SET_LAYOUT layout) const
  {
    return m_set_layouts[layout];
  }
  VkPipelineLayout GetPipelineLayout(PIPELINE_LAYOUT layout) const
  {
    return m_pipeline_layouts[layout];
  }

  StreamBuffer* GetTextureUploadBuffer() const { return m_upload_buffer.get(); }

  VkSampler GetPointSampler() const { return m_point_sampler; }
  VkSampler GetLinearSampler() const { return m_linear_sampler; }
  // The sampler for a GX sampler state, created on first use (a failed creation is cached too).
  VkSampler GetSampler(const SamplerState& info);

  // A single-subpass render pass for these attachments, created on first use.
  VkRenderPass GetRenderPass(VkFormat color_format, VkFormat depth_format, u32 multisamples,
                             VkAttachmentLoadOp load_op, u8 additional_attachment_count = 0);

  VkPipelineCache GetPipelineCache() const { return m_pipeline_cache; }

  // Destroys every per-state sampler (for anisotropy or filtering changes). None may be in use.
  void ClearSamplerCache();

  void SavePipelineCache();
  // Saves, then reopens the cache for the current host configuration.
  void ReloadPipelineCache();

private:
  struct RenderPassKey
  {
    VkFormat color;
    VkFormat depth;
    u32 samples;
    VkAttachmentLoadOp load_op;
    std::size_t extra_colors;
    auto operator<=>(const RenderPassKey&) const = default;
  };

  bool CreateSetLayouts();
  bool CreatePipelineLayouts();
  bool CreateFixedSamplers();
  bool CreateEmptyPipelineCache();
  bool OpenPipelineCacheFromDisk();
  bool HeaderMatchesDevice(const u8* data, size_t size) const;

  std::array<VkDescriptorSetLayout, NUM_DESCRIPTOR_SET_LAYOUTS> m_set_layouts = {};
  std::array<VkPipelineLayout, NUM_PIPELINE_LAYOUTS> m_pipeline_layouts = {};
  std::unique_ptr<StreamBuffer> m_upload_buffer;

  VkSampler m_point_sampler = VK_NULL_HANDLE;
  VkSampler m_linear_sampler = VK_NULL_HANDLE;
  // Ordered maps: objects are destroyed in key order.
  std::map<SamplerState, VkSampler> m_samplers;
  std::map<RenderPassKey, VkRenderPass> m_render_passes;

  VkPipelineCache m_pipeline_cache = VK_NULL_HANDLE;
  // Remembered at load time: by shutdown the game ID it was named after may be gone.
  std::string m_pipeline_cache_path;
};

extern std::unique_ptr<ObjectCache> g_object_cache;

}  // namespace Vulkan
