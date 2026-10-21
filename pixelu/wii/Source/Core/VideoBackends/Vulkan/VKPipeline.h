// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/AbstractPipeline.h"

namespace Vulkan
{
// A graphics pipeline built against the "load" render pass of its framebuffer format (the
// clear and discard passes are compatible with it). Viewport and scissor are dynamic.
class VKPipeline final : public AbstractPipeline
{
public:
  explicit VKPipeline(const AbstractPipelineConfig& config, VkPipeline pipeline,
                      VkPipelineLayout pipeline_layout, AbstractPipelineUsage usage);
  ~VKPipeline() override;

  VkPipeline GetVkPipeline() const { return m_pipeline; }
  VkPipelineLayout GetVkPipelineLayout() const { return m_layout; }
  AbstractPipelineUsage GetUsage() const { return m_usage; }

  static std::unique_ptr<VKPipeline> Create(const AbstractPipelineConfig& config);

private:
  VkPipeline m_pipeline;
  VkPipelineLayout m_layout;
  AbstractPipelineUsage m_usage;
};

}  // namespace Vulkan
