// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKPipeline.h"

#include <array>
#include <vector>

#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/VKShader.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VKVertexFormat.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

namespace Vulkan
{
namespace
{
VkCullModeFlags CullFlags(CullMode mode)
{
  switch (mode)
  {
  case CullMode::Back:
    return VK_CULL_MODE_BACK_BIT;
  case CullMode::Front:
    return VK_CULL_MODE_FRONT_BIT;
  case CullMode::All:
    return VK_CULL_MODE_FRONT_AND_BACK;
  default:
    return VK_CULL_MODE_NONE;
  }
}

VkPrimitiveTopology Topology(PrimitiveType primitive)
{
  switch (primitive)
  {
  case PrimitiveType::Points:
    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  case PrimitiveType::Lines:
    return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case PrimitiveType::Triangles:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  default:
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  }
}

// Primitive restart is only allowed on strip topologies.
bool AllowsRestart(VkPrimitiveTopology topology)
{
  switch (topology)
  {
  case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
  case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
  case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
  case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
    return true;
  default:
    return false;
  }
}

VkCompareOp DepthCompare(CompareMode mode)
{
  // Without a reversed depth range the depth values are flipped, so ordered comparisons are
  // mirrored.
  const bool flip = !g_backend_info.bSupportsReversedDepthRange;
  switch (mode)
  {
  case CompareMode::Never:
    return VK_COMPARE_OP_NEVER;
  case CompareMode::Less:
    return flip ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
  case CompareMode::Equal:
    return VK_COMPARE_OP_EQUAL;
  case CompareMode::LEqual:
    return flip ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
  case CompareMode::Greater:
    return flip ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_GREATER;
  case CompareMode::NEqual:
    return VK_COMPARE_OP_NOT_EQUAL;
  case CompareMode::GEqual:
    return flip ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL;
  case CompareMode::Always:
    return VK_COMPARE_OP_ALWAYS;
  default:
    PanicAlertFmt("Invalid compare mode {}", mode);
    return VK_COMPARE_OP_ALWAYS;
  }
}

// GX blend factors 0..7. Sources read the destination colour, destinations the source colour;
// with dual-source blending the alpha factors read the second shader output instead.
VkBlendFactor BlendFactor(u32 gx_factor, bool is_destination, bool dual_source)
{
  switch (gx_factor)
  {
  case 0:
    return VK_BLEND_FACTOR_ZERO;
  case 1:
    return VK_BLEND_FACTOR_ONE;
  case 2:
    return is_destination ? VK_BLEND_FACTOR_SRC_COLOR : VK_BLEND_FACTOR_DST_COLOR;
  case 3:
    return is_destination ? VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR :
                            VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case 4:
    return dual_source ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA;
  case 5:
    return dual_source ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA :
                         VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case 6:
    return VK_BLEND_FACTOR_DST_ALPHA;
  default:
    return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  }
}

VkPipelineRasterizationStateCreateInfo RasterState(const RasterizationState& state)
{
  VkPipelineRasterizationStateCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  info.depthClampEnable = g_backend_info.bSupportsDepthClamp;
  info.polygonMode = VK_POLYGON_MODE_FILL;
  info.cullMode = CullFlags(state.cull_mode);
  info.frontFace = VK_FRONT_FACE_CLOCKWISE;
  info.lineWidth = 1.0f;
  return info;
}

VkPipelineMultisampleStateCreateInfo SampleState(const FramebufferState& state)
{
  VkPipelineMultisampleStateCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  info.rasterizationSamples = static_cast<VkSampleCountFlagBits>(state.samples.Value());
  info.sampleShadingEnable = static_cast<bool>(state.per_sample_shading);
  info.minSampleShading = 1.0f;
  return info;
}

VkPipelineDepthStencilStateCreateInfo DepthTest(const DepthState& state)
{
  VkPipelineDepthStencilStateCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  info.depthTestEnable = state.test_enable;
  info.depthWriteEnable = state.update_enable;
  info.depthCompareOp = DepthCompare(state.func);
  info.minDepthBounds = 0.0f;
  info.maxDepthBounds = 1.0f;
  return info;
}

VkPipelineColorBlendAttachmentState AttachmentBlend(const BlendingState& state)
{
  const bool dual = state.use_dual_src;
  VkPipelineColorBlendAttachmentState blend = {};
  blend.blendEnable = static_cast<VkBool32>(state.blend_enable);
  blend.srcColorBlendFactor = BlendFactor(u32(state.src_factor.Value()), false, dual);
  blend.dstColorBlendFactor = BlendFactor(u32(state.dst_factor.Value()), true, dual);
  blend.colorBlendOp = state.subtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = BlendFactor(u32(state.src_factor_alpha.Value()), false, dual);
  blend.dstAlphaBlendFactor = BlendFactor(u32(state.dst_factor_alpha.Value()), true, dual);
  blend.alphaBlendOp = state.subtract_alpha ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;

  VkColorComponentFlags mask = 0;
  if (state.color_update)
    mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
  if (state.alpha_update)
    mask |= VK_COLOR_COMPONENT_A_BIT;
  blend.colorWriteMask = mask;
  return blend;
}

VkPipelineColorBlendStateCreateInfo BlendState(const BlendingState& state,
                                               const VkPipelineColorBlendAttachmentState* attachments,
                                               u32 attachment_count)
{
  // Devices without logic ops (some mobile GPUs) just draw without them.
  const bool logic = state.logic_op_enable && g_backend_info.bSupportsLogicOp;

  VkPipelineColorBlendStateCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  info.logicOpEnable = logic ? VK_TRUE : VK_FALSE;
  // GX logic op numbers match VkLogicOp one to one.
  info.logicOp = logic ? static_cast<VkLogicOp>(u32(state.logic_mode.Value())) : VK_LOGIC_OP_CLEAR;
  info.attachmentCount = attachment_count;
  info.pAttachments = attachments;
  info.blendConstants[0] = 1.0f;
  info.blendConstants[1] = 1.0f;
  info.blendConstants[2] = 1.0f;
  info.blendConstants[3] = 1.0f;
  return info;
}

VkPipelineShaderStageCreateInfo Stage(VkShaderStageFlagBits stage, const AbstractShader* shader)
{
  return {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          nullptr,
          0,
          stage,
          static_cast<const VKShader*>(shader)->GetShaderModule(),
          "main",
          nullptr};
}

bool LayoutFor(AbstractPipelineUsage usage, VkPipelineLayout* layout)
{
  switch (usage)
  {
  case AbstractPipelineUsage::GX:
    *layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_STANDARD);
    return true;
  case AbstractPipelineUsage::GXUber:
    *layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_UBER);
    return true;
  case AbstractPipelineUsage::Utility:
    *layout = g_object_cache->GetPipelineLayout(PIPELINE_LAYOUT_UTILITY);
    return true;
  default:
    return false;
  }
}

// Fixed parts shared by every pipeline. The viewport and scissor here are placeholders (both
// are dynamic). The depth clamp range is chained in when the device clamps to a user range.
const VkPipelineViewportStateCreateInfo& SharedViewportState()
{
  static const VkDepthClampRangeEXT range = {0.0f, MAX_EFB_DEPTH};
  static const VkPipelineViewportDepthClampControlCreateInfoEXT clamp = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT, nullptr,
      VK_DEPTH_CLAMP_MODE_USER_DEFINED_RANGE_EXT, &range};
  static const VkViewport viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
  static const VkRect2D scissor = {{0, 0}, {1, 1}};
  // Decided once, on the first pipeline.
  static const VkPipelineViewportStateCreateInfo state = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      g_backend_info.bSupportsUnrestrictedDepthRange ? &clamp : nullptr,
      0,
      1,
      &viewport,
      1,
      &scissor};
  return state;
}

const VkPipelineDynamicStateCreateInfo& SharedDynamicState()
{
  static constexpr VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                               VK_DYNAMIC_STATE_SCISSOR};
  static const VkPipelineDynamicStateCreateInfo state = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0,
      static_cast<u32>(std::size(dynamic)), dynamic};
  return state;
}
}  // namespace

VKPipeline::VKPipeline(const AbstractPipelineConfig& config, VkPipeline pipeline,
                       VkPipelineLayout pipeline_layout, AbstractPipelineUsage usage)
    : AbstractPipeline(config), m_pipeline(pipeline), m_layout(pipeline_layout), m_usage(usage)
{
}

VKPipeline::~VKPipeline()
{
  vkDestroyPipeline(g_vulkan_context->GetDevice(), m_pipeline, nullptr);
}

std::unique_ptr<VKPipeline> VKPipeline::Create(const AbstractPipelineConfig& config)
{
  DEBUG_ASSERT(config.vertex_shader && config.pixel_shader);
  const FramebufferState& fb = config.framebuffer_state;

  const VkRenderPass render_pass = g_object_cache->GetRenderPass(
      VKTexture::GetVkFormatForHostTextureFormat(fb.color_texture_format),
      VKTexture::GetVkFormatForHostTextureFormat(fb.depth_texture_format), fb.samples,
      VK_ATTACHMENT_LOAD_OP_LOAD, static_cast<u8>(fb.additional_color_attachment_count));
  if (render_pass == VK_NULL_HANDLE)
  {
    PanicAlertFmt("Failed to get render pass");
    return nullptr;
  }

  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (!LayoutFor(config.usage, &layout))
  {
    PanicAlertFmt("Unknown pipeline layout.");
    return nullptr;
  }

  // Draws without a vertex format fetch nothing from vertex buffers.
  static const VkPipelineVertexInputStateCreateInfo no_vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 0, nullptr, 0,
      nullptr};
  const VkPipelineVertexInputStateCreateInfo& vertex_input =
      config.vertex_format != nullptr ?
          static_cast<const VertexFormat*>(config.vertex_format)->GetVertexInputStateInfo() :
          no_vertex_input;

  VkPipelineInputAssemblyStateCreateInfo assembly = {};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = Topology(config.rasterization_state.primitive);
  assembly.primitiveRestartEnable =
      (g_backend_info.bSupportsPrimitiveRestart && AllowsRestart(assembly.topology)) ? VK_TRUE :
                                                                                        VK_FALSE;

  std::array<VkPipelineShaderStageCreateInfo, 3> stages;
  u32 stage_count = 0;
  if (config.vertex_shader != nullptr)
    stages[stage_count++] = Stage(VK_SHADER_STAGE_VERTEX_BIT, config.vertex_shader);
  if (config.geometry_shader != nullptr)
    stages[stage_count++] = Stage(VK_SHADER_STAGE_GEOMETRY_BIT, config.geometry_shader);
  if (config.pixel_shader != nullptr)
    stages[stage_count++] = Stage(VK_SHADER_STAGE_FRAGMENT_BIT, config.pixel_shader);

  const VkPipelineRasterizationStateCreateInfo raster = RasterState(config.rasterization_state);
  const VkPipelineMultisampleStateCreateInfo multisample = SampleState(fb);
  const VkPipelineDepthStencilStateCreateInfo depth = DepthTest(config.depth_state);

  // Every color attachment blends the same way.
  const u32 extra_colors = static_cast<u8>(fb.additional_color_attachment_count);
  const std::vector<VkPipelineColorBlendAttachmentState> attachment_blends(
      1 + extra_colors, AttachmentBlend(config.blending_state));
  const VkPipelineColorBlendStateCreateInfo blend =
      BlendState(config.blending_state, attachment_blends.data(),
                 static_cast<u32>(attachment_blends.size()));

  VkGraphicsPipelineCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = stage_count;
  info.pStages = stages.data();
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &SharedViewportState();
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &blend;
  info.pDynamicState = &SharedDynamicState();
  info.layout = layout;
  info.renderPass = render_pass;
  info.basePipelineHandle = VK_NULL_HANDLE;
  info.basePipelineIndex = -1;

  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult res = vkCreateGraphicsPipelines(
      g_vulkan_context->GetDevice(), g_object_cache->GetPipelineCache(), 1, &info, nullptr, &pipeline);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateGraphicsPipelines failed: ");
    return nullptr;
  }
  return std::make_unique<VKPipeline>(config, pipeline, layout, config.usage);
}
}  // namespace Vulkan
