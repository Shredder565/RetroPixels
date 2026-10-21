// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKShader.h"

#include <cstring>
#include <optional>

#include "Common/Align.h"

#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/ShaderCompiler.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
using ShaderCompiler::SPIRVCodeType;
using ShaderCompiler::SPIRVCodeVector;

VKShader::VKShader(ShaderStage stage, std::vector<u32> spv, VkShaderModule mod,
                   std::string_view name)
    : AbstractShader(stage), m_spv(std::move(spv)), m_module(mod), m_name(name)
{
  SetDebugName(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<u64>(m_module));
}

VKShader::VKShader(std::vector<u32> spv, VkPipeline compute_pipeline, std::string_view name)
    : AbstractShader(ShaderStage::Compute), m_spv(std::move(spv)),
      m_compute_pipeline(compute_pipeline), m_name(name)
{
  SetDebugName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(m_compute_pipeline));
}

void VKShader::SetDebugName(VkObjectType type, u64 handle)
{
  if (m_name.empty() || !g_backend_info.bSupportsSettingObjectNames)
    return;
  VkDebugUtilsObjectNameInfoEXT label = {};
  label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
  label.objectType = type;
  label.objectHandle = handle;
  label.pObjectName = m_name.data();
  vkSetDebugUtilsObjectNameEXT(g_vulkan_context->GetDevice(), &label);
}

VKShader::~VKShader()
{
  const VkDevice device = g_vulkan_context->GetDevice();
  if (m_stage == ShaderStage::Compute)
    vkDestroyPipeline(device, m_compute_pipeline, nullptr);
  else
    vkDestroyShaderModule(device, m_module, nullptr);
}

AbstractShader::BinaryData VKShader::GetBinary() const
{
  const size_t bytes = m_spv.size() * sizeof(u32);
  BinaryData blob(bytes);
  std::memcpy(blob.data(), m_spv.data(), bytes);
  return blob;
}

namespace
{
std::unique_ptr<VKShader> Instantiate(ShaderStage stage, SPIRVCodeVector spv, std::string_view name)
{
  const VkDevice device = g_vulkan_context->GetDevice();

  VkShaderModuleCreateInfo module_info = {};
  module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  module_info.codeSize = spv.size() * sizeof(u32);
  module_info.pCode = spv.data();

  VkShaderModule shader_module = VK_NULL_HANDLE;
  VkResult res = vkCreateShaderModule(device, &module_info, nullptr, &shader_module);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateShaderModule failed: ");
    return nullptr;
  }

  // Graphics stages are linked later, in pipelines.
  if (stage != ShaderStage::Compute)
    return std::make_unique<VKShader>(stage, std::move(spv), shader_module, name);

  VkComputePipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                         nullptr,
                         0,
                         VK_SHADER_STAGE_COMPUTE_BIT,
                         shader_module,
                         "main",
                         nullptr};
  pipeline_info.layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_COMPUTE);
  pipeline_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_info.basePipelineIndex = -1;

  VkPipeline pipeline = VK_NULL_HANDLE;
  res = vkCreateComputePipelines(device, g_object_cache->GetPipelineCache(), 1, &pipeline_info,
                                 nullptr, &pipeline);
  // The pipeline holds everything it needs; the module can go either way.
  vkDestroyShaderModule(device, shader_module, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateComputePipelines failed: ");
    return nullptr;
  }
  return std::make_unique<VKShader>(std::move(spv), pipeline, name);
}
}  // namespace

std::unique_ptr<VKShader> VKShader::CreateFromSource(ShaderStage stage, std::string_view source,
                                                     VideoCommon::ShaderIncluder* shader_includer,
                                                     std::string_view name)
{
  std::optional<SPIRVCodeVector> spv;
  if (stage == ShaderStage::Vertex)
    spv = ShaderCompiler::CompileVertexShader(source, shader_includer);
  else if (stage == ShaderStage::Geometry)
    spv = ShaderCompiler::CompileGeometryShader(source, shader_includer);
  else if (stage == ShaderStage::Pixel)
    spv = ShaderCompiler::CompileFragmentShader(source, shader_includer);
  else if (stage == ShaderStage::Compute)
    spv = ShaderCompiler::CompileComputeShader(source, shader_includer);

  if (!spv.has_value())
    return nullptr;
  return Instantiate(stage, std::move(*spv), name);
}

std::unique_ptr<VKShader> VKShader::CreateFromBinary(ShaderStage stage, const void* data,
                                                     size_t length, std::string_view name)
{
  // Round up to whole words; a partial last word is zero-padded.
  SPIRVCodeVector spv(Common::AlignUp(length, sizeof(SPIRVCodeType)) / sizeof(SPIRVCodeType));
  if (length != 0)
    std::memcpy(spv.data(), data, length);
  return Instantiate(stage, std::move(spv), name);
}

}  // namespace Vulkan
