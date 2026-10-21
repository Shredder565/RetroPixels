// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/AbstractShader.h"

namespace Vulkan
{
// A compiled shader. Graphics stages keep their VkShaderModule for pipeline creation; compute
// shaders are turned into their pipeline at once (the module is not kept).
class VKShader final : public AbstractShader
{
public:
  VKShader(ShaderStage stage, std::vector<u32> spv, VkShaderModule mod, std::string_view name);
  VKShader(std::vector<u32> spv, VkPipeline compute_pipeline, std::string_view name);
  ~VKShader() override;

  VkShaderModule GetShaderModule() const { return m_module; }
  VkPipeline GetComputePipeline() const { return m_compute_pipeline; }
  // The SPIR-V words as bytes.
  BinaryData GetBinary() const override;

  static std::unique_ptr<VKShader> CreateFromSource(ShaderStage stage, std::string_view source,
                                                    VideoCommon::ShaderIncluder* shader_includer,
                                                    std::string_view name);
  static std::unique_ptr<VKShader> CreateFromBinary(ShaderStage stage, const void* data,
                                                    size_t length, std::string_view name);

private:
  void SetDebugName(VkObjectType type, u64 handle);

  std::vector<u32> m_spv;
  VkShaderModule m_module = VK_NULL_HANDLE;
  VkPipeline m_compute_pipeline = VK_NULL_HANDLE;
  std::string m_name;
};

}  // namespace Vulkan
