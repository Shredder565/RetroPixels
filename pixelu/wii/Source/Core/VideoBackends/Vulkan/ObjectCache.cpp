// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/ObjectCache.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/LinearDiskCache.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/VKStreamBuffer.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/Constants.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VideoCommon.h"

namespace Vulkan
{
std::unique_ptr<ObjectCache> g_object_cache;

namespace
{
VkDevice Device()
{
  return g_vulkan_context->GetDevice();
}

// Appends `count` single-descriptor bindings numbered first..first+count-1.
void AddBindings(std::vector<VkDescriptorSetLayoutBinding>* out, u32 first, u32 count,
                 VkDescriptorType type, VkShaderStageFlags stages)
{
  for (u32 n = 0; n < count; ++n)
    out->push_back({first + n, type, 1, stages, nullptr});
}

// The header the Vulkan spec puts at the start of VK_PIPELINE_CACHE_HEADER_VERSION_ONE data
// (stored little-endian, which every host we build for is).
#pragma pack(push, 4)
struct PipelineCacheHeader
{
  u32 length;
  u32 version;
  u32 vendor;
  u32 device;
  u8 uuid[VK_UUID_SIZE];
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<PipelineCacheHeader>);

// Keeps the payload of the (single) entry in a pipeline cache file.
class BlobReader final : public Common::LinearDiskCacheReader<u32, u8>
{
public:
  explicit BlobReader(std::vector<u8>* blob) : m_blob(blob) {}
  void Read(const u32&, const u8* value, u32 value_size) override
  {
    m_blob->assign(value, value + value_size);
  }

private:
  std::vector<u8>* m_blob;
};

// Reads and throws away every entry; used to open a cache file for appending.
class DiscardReader final : public Common::LinearDiskCacheReader<u32, u8>
{
public:
  void Read(const u32&, const u8*, u32) override {}
};
}  // namespace

ObjectCache::ObjectCache() = default;

ObjectCache::~ObjectCache()
{
  const VkDevice device = Device();

  vkDestroyPipelineCache(device, m_pipeline_cache, nullptr);
  m_pipeline_cache = VK_NULL_HANDLE;

  ClearSamplerCache();
  for (VkSampler* fixed : {&m_point_sampler, &m_linear_sampler})
  {
    if (*fixed == VK_NULL_HANDLE)
      continue;
    vkDestroySampler(device, *fixed, nullptr);
    *fixed = VK_NULL_HANDLE;
  }

  for (VkPipelineLayout layout : m_pipeline_layouts)
  {
    if (layout != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device, layout, nullptr);
  }
  for (VkDescriptorSetLayout layout : m_set_layouts)
  {
    if (layout != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device, layout, nullptr);
  }

  for (const auto& [key, pass] : m_render_passes)
    vkDestroyRenderPass(device, pass, nullptr);
  m_render_passes.clear();
}

bool ObjectCache::Initialize()
{
  if (!CreateSetLayouts() || !CreatePipelineLayouts() || !CreateFixedSamplers())
    return false;

  m_upload_buffer =
      StreamBuffer::Create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, TEXTURE_UPLOAD_BUFFER_SIZE);
  if (m_upload_buffer == nullptr)
  {
    PanicAlertFmt("Failed to create texture upload buffer");
    return false;
  }

  return g_ActiveConfig.bShaderCache ? OpenPipelineCacheFromDisk() : CreateEmptyPipelineCache();
}

void ObjectCache::Shutdown()
{
  if (m_pipeline_cache != VK_NULL_HANDLE && g_ActiveConfig.bShaderCache)
    SavePipelineCache();
}

void ObjectCache::ClearSamplerCache()
{
  const VkDevice device = Device();
  for (const auto& [state, sampler] : m_samplers)
  {
    if (sampler != VK_NULL_HANDLE)
      vkDestroySampler(device, sampler, nullptr);
  }
  m_samplers.clear();
}

bool ObjectCache::CreateSetLayouts()
{
  static_assert(VideoCommon::MAX_PIXEL_SHADER_SAMPLERS == 16,
                "The sampler set below assumes 16 pixel shader samplers");
  constexpr u32 ARRAYED_SAMPLERS = 8;
  constexpr VkShaderStageFlags FRAG = VK_SHADER_STAGE_FRAGMENT_BIT;
  constexpr VkShaderStageFlags COMP = VK_SHADER_STAGE_COMPUTE_BIT;
  constexpr VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  constexpr VkDescriptorType DYN_UBO = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  constexpr VkDescriptorType TEXEL = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;

  std::array<std::vector<VkDescriptorSetLayoutBinding>, NUM_DESCRIPTOR_SET_LAYOUTS> sets;

  // GX uniforms: PS, VS, custom, GS. The GS buffer is last so it can be cut off below.
  auto& gx_ubos = sets[DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS];
  AddBindings(&gx_ubos, UBO_DESCRIPTOR_SET_BINDING_PS, 1, DYN_UBO, FRAG);
  AddBindings(&gx_ubos, UBO_DESCRIPTOR_SET_BINDING_VS, 1, DYN_UBO,
              VK_SHADER_STAGE_VERTEX_BIT | FRAG);
  AddBindings(&gx_ubos, UBO_DESCRIPTOR_SET_BINDING_CUST, 1, DYN_UBO,
              VK_SHADER_STAGE_VERTEX_BIT | FRAG);
  AddBindings(&gx_ubos, UBO_DESCRIPTOR_SET_BINDING_GS, 1, DYN_UBO, VK_SHADER_STAGE_GEOMETRY_BIT);

  // GX samplers: one 8-element array, then the remaining samplers one binding each.
  auto& gx_samplers = sets[DESCRIPTOR_SET_LAYOUT_STANDARD_SAMPLERS];
  gx_samplers.push_back({0, CIS, ARRAYED_SAMPLERS, FRAG, nullptr});
  AddBindings(&gx_samplers, ARRAYED_SAMPLERS,
              VideoCommon::MAX_PIXEL_SHADER_SAMPLERS - ARRAYED_SAMPLERS, CIS, FRAG);

  // Storage buffers: bounding box, then the dynamic vertex loader's vertices (last, so it can
  // be cut off).
  auto& gx_ssbos = sets[DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS];
  AddBindings(&gx_ssbos, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FRAG);
  AddBindings(&gx_ssbos, 1, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);

  AddBindings(&sets[DESCRIPTOR_SET_LAYOUT_UTILITY_UNIFORM_BUFFER], 0, 1, DYN_UBO,
              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | FRAG);

  auto& util_samplers = sets[DESCRIPTOR_SET_LAYOUT_UTILITY_SAMPLERS];
  AddBindings(&util_samplers, 0, 8, CIS, FRAG);
  AddBindings(&util_samplers, 8, 1, TEXEL, FRAG);

  auto& compute = sets[DESCRIPTOR_SET_LAYOUT_COMPUTE];
  AddBindings(&compute, 0, 1, DYN_UBO, COMP);
  AddBindings(&compute, 1, 8, CIS, COMP);
  AddBindings(&compute, 9, 2, TEXEL, COMP);
  AddBindings(&compute, 11, 8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, COMP);

  // Line/point expansion in the vertex shader reads the GS uniforms from the VS; without a
  // geometry stage at all the GS buffer is not needed.
  VkDescriptorSetLayoutBinding& gs_ubo = gx_ubos[UBO_DESCRIPTOR_SET_BINDING_GS];
  if (g_ActiveConfig.UseVSForLinePointExpand())
  {
    gs_ubo.stageFlags = g_backend_info.bSupportsGeometryShaders ?
                            (gs_ubo.stageFlags | VK_SHADER_STAGE_VERTEX_BIT) :
                            VkShaderStageFlags{VK_SHADER_STAGE_VERTEX_BIT};
  }
  else if (!g_backend_info.bSupportsGeometryShaders)
  {
    gx_ubos.pop_back();
  }
  if (!g_backend_info.bSupportsDynamicVertexLoader)
    gx_ssbos.pop_back();

  for (size_t index = 0; index < sets.size(); ++index)
  {
    const VkDescriptorSetLayoutCreateInfo info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
        static_cast<u32>(sets[index].size()), sets[index].data()};
    const VkResult res = vkCreateDescriptorSetLayout(Device(), &info, nullptr, &m_set_layouts[index]);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateDescriptorSetLayout failed: ");
      return false;
    }
  }
  return true;
}

bool ObjectCache::CreatePipelineLayouts()
{
  const VkDescriptorSetLayout gx_sets[] = {
      m_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_UNIFORM_BUFFERS],
      m_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SAMPLERS],
      m_set_layouts[DESCRIPTOR_SET_LAYOUT_STANDARD_SHADER_STORAGE_BUFFERS],
  };
  const VkDescriptorSetLayout utility_sets[] = {
      m_set_layouts[DESCRIPTOR_SET_LAYOUT_UTILITY_UNIFORM_BUFFER],
      m_set_layouts[DESCRIPTOR_SET_LAYOUT_UTILITY_SAMPLERS],
  };
  const VkDescriptorSetLayout compute_sets[] = {m_set_layouts[DESCRIPTOR_SET_LAYOUT_COMPUTE]};

  // The storage buffer set is the last one, and is left out when nothing uses it.
  const bool standard_needs_ssbo =
      g_backend_info.bSupportsBBox || g_ActiveConfig.UseVSForLinePointExpand();
  const bool uber_needs_ssbo = standard_needs_ssbo || g_backend_info.bSupportsDynamicVertexLoader;

  struct LayoutSpec
  {
    const VkDescriptorSetLayout* sets;
    u32 count;
  };
  std::array<LayoutSpec, NUM_PIPELINE_LAYOUTS> specs = {};
  specs[PIPELINE_LAYOUT_STANDARD] = {gx_sets, standard_needs_ssbo ? 3u : 2u};
  specs[PIPELINE_LAYOUT_UBER] = {gx_sets, uber_needs_ssbo ? 3u : 2u};
  specs[PIPELINE_LAYOUT_UTILITY] = {utility_sets, 2};
  specs[PIPELINE_LAYOUT_COMPUTE] = {compute_sets, 1};

  for (size_t index = 0; index < specs.size(); ++index)
  {
    const VkPipelineLayoutCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                             nullptr,
                                             0,
                                             specs[index].count,
                                             specs[index].sets,
                                             0,
                                             nullptr};
    const VkResult res = vkCreatePipelineLayout(Device(), &info, nullptr, &m_pipeline_layouts[index]);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreatePipelineLayout failed: ");
      return false;
    }
  }
  return true;
}

bool ObjectCache::CreateFixedSamplers()
{
  VkSamplerCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  info.maxAnisotropy = 1.0f;
  info.compareOp = VK_COMPARE_OP_ALWAYS;
  info.minLod = std::numeric_limits<float>::min();
  info.maxLod = std::numeric_limits<float>::max();
  info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

  struct Variant
  {
    VkSampler* out;
    VkFilter filter;
    VkSamplerMipmapMode mip;
  };
  const Variant variants[] = {
      {&m_point_sampler, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST},
      {&m_linear_sampler, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR},
  };
  for (const Variant& v : variants)
  {
    info.magFilter = v.filter;
    info.minFilter = v.filter;
    info.mipmapMode = v.mip;
    const VkResult res = vkCreateSampler(Device(), &info, nullptr, v.out);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");
      return false;
    }
  }
  return true;
}

VkSampler ObjectCache::GetSampler(const SamplerState& info)
{
  if (const auto found = m_samplers.find(info); found != m_samplers.end())
    return found->second;

  const auto filter = [](auto mode) {
    return u32(mode) == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  };
  // GX wrap modes: clamp, repeat, mirror; the reserved value 3 behaves as repeat.
  const auto address = [](auto mode) {
    switch (u32(mode))
    {
    case 0:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 2:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
  };

  VkSamplerCreateInfo create = {};
  create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  create.magFilter = filter(info.tm0.mag_filter.Value());
  create.minFilter = filter(info.tm0.min_filter.Value());
  create.mipmapMode = u32(info.tm0.mipmap_filter.Value()) == 0 ? VK_SAMPLER_MIPMAP_MODE_NEAREST :
                                                                  VK_SAMPLER_MIPMAP_MODE_LINEAR;
  create.addressModeU = address(info.tm0.wrap_u.Value());
  create.addressModeV = address(info.tm0.wrap_v.Value());
  create.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  create.mipLodBias = static_cast<float>(info.tm0.lod_bias) / 256.0f;
  create.maxAnisotropy = 0.0f;
  create.compareOp = VK_COMPARE_OP_ALWAYS;
  create.minLod = static_cast<float>(info.tm1.min_lod) / 16.0f;
  create.maxLod = static_cast<float>(info.tm1.max_lod) / 16.0f;
  create.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

  const u32 aniso_log2 = info.tm0.anisotropic_filtering;
  if (aniso_log2 != 0 && g_vulkan_context->SupportsAnisotropicFiltering())
  {
    create.anisotropyEnable = VK_TRUE;
    create.maxAnisotropy = std::min(static_cast<float>(1 << aniso_log2),
                                    g_vulkan_context->GetMaxSamplerAnisotropy());
  }

  VkSampler sampler = VK_NULL_HANDLE;
  const VkResult res = vkCreateSampler(Device(), &create, nullptr, &sampler);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkCreateSampler failed: ");

  // A null handle is remembered as well, so a failing state is not retried every draw.
  m_samplers.emplace(info, sampler);
  return sampler;
}

VkRenderPass ObjectCache::GetRenderPass(VkFormat color_format, VkFormat depth_format,
                                        u32 multisamples, VkAttachmentLoadOp load_op,
                                        u8 additional_attachment_count)
{
  const RenderPassKey key{color_format, depth_format, multisamples, load_op,
                          additional_attachment_count};
  if (const auto found = m_render_passes.find(key); found != m_render_passes.end())
    return found->second;

  const auto samples = static_cast<VkSampleCountFlagBits>(multisamples);
  std::vector<VkAttachmentDescription> attachments;
  std::vector<VkAttachmentReference> color_refs;
  VkAttachmentReference depth_ref = {};
  bool has_depth = false;

  const auto add_attachment = [&](VkFormat format, VkImageLayout layout) {
    const u32 slot = static_cast<u32>(attachments.size());
    attachments.push_back({0, format, samples, load_op, VK_ATTACHMENT_STORE_OP_STORE,
                           VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                           layout, layout});
    return VkAttachmentReference{slot, layout};
  };

  if (color_format != VK_FORMAT_UNDEFINED)
    color_refs.push_back(add_attachment(color_format, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
  if (depth_format != VK_FORMAT_UNDEFINED)
  {
    depth_ref = add_attachment(depth_format, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    has_depth = true;
  }
  // Extra color outputs share the main color format.
  for (u32 extra = 0; extra < additional_attachment_count; ++extra)
    color_refs.push_back(add_attachment(color_format, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));

  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = static_cast<u32>(color_refs.size());
  subpass.pColorAttachments = color_refs.empty() ? nullptr : color_refs.data();
  subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;

  VkRenderPassCreateInfo pass_info = {};
  pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  pass_info.attachmentCount = static_cast<u32>(attachments.size());
  pass_info.pAttachments = attachments.data();
  pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;

  VkRenderPass pass = VK_NULL_HANDLE;
  const VkResult res = vkCreateRenderPass(Device(), &pass_info, nullptr, &pass);
  if (res != VK_SUCCESS)
  {
    // Not remembered: the next request tries again.
    LOG_VULKAN_ERROR(res, "vkCreateRenderPass failed: ");
    return VK_NULL_HANDLE;
  }

  m_render_passes.emplace(key, pass);
  return pass;
}

bool ObjectCache::CreateEmptyPipelineCache()
{
  // One cache file is shared by all games; drivers only build pipelines from it on lookup.
  m_pipeline_cache_path = GetDiskShaderCacheFileName(APIType::Vulkan, "Pipeline", false, true);

  VkPipelineCacheCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  const VkResult res = vkCreatePipelineCache(Device(), &info, nullptr, &m_pipeline_cache);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreatePipelineCache failed: ");
    return false;
  }
  return true;
}

bool ObjectCache::OpenPipelineCacheFromDisk()
{
  // Remember the name now: when saving at shutdown the game ID it depends on is gone.
  m_pipeline_cache_path = GetDiskShaderCacheFileName(APIType::Vulkan, "Pipeline", false, true);

  std::vector<u8> blob;
  {
    Common::LinearDiskCache<u32, u8> file;
    BlobReader reader(&blob);
    if (file.OpenAndRead(m_pipeline_cache_path, reader) != 1)
      blob.clear();
  }

  if (!blob.empty() && !HeaderMatchesDevice(blob.data(), blob.size()))
  {
    // Stale data from another driver or GPU: remove it so it is not tried again.
    File::Delete(m_pipeline_cache_path);
    return CreateEmptyPipelineCache();
  }

  VkPipelineCacheCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
  info.initialDataSize = blob.size();
  info.pInitialData = blob.data();
  const VkResult res = vkCreatePipelineCache(Device(), &info, nullptr, &m_pipeline_cache);
  if (res == VK_SUCCESS)
    return true;

  LOG_VULKAN_ERROR(res, "vkCreatePipelineCache failed, trying empty cache: ");
  return CreateEmptyPipelineCache();
}

bool ObjectCache::HeaderMatchesDevice(const u8* data, size_t size) const
{
  constexpr const char* PREFIX = "Pipeline cache failed validation: ";
  if (size < sizeof(PipelineCacheHeader))
  {
    ERROR_LOG_FMT(VIDEO, "{}Invalid header", PREFIX);
    return false;
  }

  PipelineCacheHeader header;
  std::memcpy(&header, data, sizeof(header));
  const VulkanContext::PhysicalDeviceInfo& device = g_vulkan_context->GetDeviceInfo();

  if (header.length < sizeof(PipelineCacheHeader))
  {
    ERROR_LOG_FMT(VIDEO, "{}Invalid header length", PREFIX);
    return false;
  }
  if (header.version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
  {
    ERROR_LOG_FMT(VIDEO, "{}Invalid header version", PREFIX);
    return false;
  }
  if (header.vendor != device.vendorID)
  {
    ERROR_LOG_FMT(VIDEO, "{}Incorrect vendor ID (file: {:#X}, device: {:#X})", PREFIX,
                  header.vendor, device.vendorID);
    return false;
  }
  if (header.device != device.deviceID)
  {
    ERROR_LOG_FMT(VIDEO, "{}Incorrect device ID (file: {:#X}, device: {:#X})", PREFIX,
                  header.device, device.deviceID);
    return false;
  }
  if (std::memcmp(header.uuid, device.pipelineCacheUUID, VK_UUID_SIZE) != 0)
  {
    ERROR_LOG_FMT(VIDEO, "{}Incorrect UUID", PREFIX);
    return false;
  }
  return true;
}

void ObjectCache::SavePipelineCache()
{
  // Ask for the size first, then the data.
  size_t size = 0;
  VkResult res = vkGetPipelineCacheData(Device(), m_pipeline_cache, &size, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
    return;
  }
  std::vector<u8> blob(size);
  res = vkGetPipelineCacheData(Device(), m_pipeline_cache, &size, blob.data());
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPipelineCacheData failed: ");
    return;
  }

  // The file holds one entry (key 1) with the whole blob; start from an empty file.
  File::Delete(m_pipeline_cache_path);
  Common::LinearDiskCache<u32, u8> file;
  DiscardReader discard;
  file.OpenAndRead(m_pipeline_cache_path, discard);
  file.Append(1, blob.data(), static_cast<u32>(blob.size()));
  file.Close();
}

void ObjectCache::ReloadPipelineCache()
{
  SavePipelineCache();
  // The previous VkPipelineCache handle is simply replaced.
  if (g_ActiveConfig.bShaderCache)
    OpenPipelineCacheFromDisk();
  else
    CreateEmptyPipelineCache();
}

}  // namespace Vulkan
