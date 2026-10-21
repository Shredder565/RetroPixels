// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VulkanContext.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <utility>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/VideoCommon.h"

namespace Vulkan
{
std::unique_ptr<VulkanContext> g_vulkan_context;

namespace
{
constexpr const char* KHRONOS_VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

// Pushes `link` right behind `head` in a pNext chain.
template <typename Head, typename Link>
void ChainBehind(Head& head, Link& link)
{
  link.pNext = head.pNext;
  head.pNext = &link;
}

bool HasNamed(const std::vector<VkExtensionProperties>& list, std::string_view name)
{
  return std::ranges::any_of(list, [name](const VkExtensionProperties& p) {
    return std::string_view{p.extensionName} == name;
  });
}

bool HasNamed(const std::vector<VkLayerProperties>& list, std::string_view name)
{
  return std::ranges::any_of(
      list, [name](const VkLayerProperties& p) { return std::string_view{p.layerName} == name; });
}

// The usual two-call enumeration of instance extensions, from the loader or from one layer.
// Only a failing size query is reported; the second call is expected to succeed.
bool QueryInstanceExtensions(const char* layer, std::vector<VkExtensionProperties>* out,
                             const char* failure_message)
{
  u32 count = 0;
  VkResult res = vkEnumerateInstanceExtensionProperties(layer, &count, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, failure_message);
    return false;
  }
  out->resize(count);
  res = vkEnumerateInstanceExtensionProperties(layer, &count, out->data());
  ASSERT(res == VK_SUCCESS);
  return true;
}

// The highest instance API version the backend uses: 1.2, else 1.1, else 1.0.
u32 PickInstanceApiVersion()
{
  u32 loader_version = 0;
  if (!vkEnumerateInstanceVersion || vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS)
  {
    WARN_LOG_FMT(HOST_GPU, "Using Vulkan 1.0");
    return VK_API_VERSION_1_0;
  }

  u32 chosen = VK_API_VERSION_1_0;
  for (u32 candidate : {VK_API_VERSION_1_2, VK_API_VERSION_1_1})
  {
    if (loader_version >= candidate)
    {
      chosen = candidate;
      break;
    }
  }
  WARN_LOG_FMT(HOST_GPU, "Using Vulkan 1.{}, supported: {}.{}", VK_VERSION_MINOR(chosen),
               VK_VERSION_MAJOR(loader_version), VK_VERSION_MINOR(loader_version));
  return chosen;
}

VKAPI_ATTR VkBool32 VKAPI_CALL ForwardDebugMessage(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/)
{
  const std::string text = fmt::format("Vulkan debug message: {}", data->pMessage);
  switch (severity)
  {
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
    ERROR_LOG_FMT(HOST_GPU, "{}", text);
    break;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
    WARN_LOG_FMT(HOST_GPU, "{}", text);
    break;
  case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
    INFO_LOG_FMT(HOST_GPU, "{}", text);
    break;
  default:
    DEBUG_LOG_FMT(HOST_GPU, "{}", text);
    break;
  }
  // Never abort the call that produced the message.
  return VK_FALSE;
}

// Open-source Mesa drivers identify themselves through VkDriverId (Vulkan 1.2).
bool IsMesaDriverId(VkDriverId id)
{
  static constexpr std::array MESA_IDS = {
      VK_DRIVER_ID_MESA_RADV,    VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA,
      VK_DRIVER_ID_MESA_LLVMPIPE, VK_DRIVER_ID_MESA_TURNIP,
      VK_DRIVER_ID_MESA_V3DV,    VK_DRIVER_ID_MESA_PANVK,
      VK_DRIVER_ID_MESA_VENUS,   VK_DRIVER_ID_MESA_DOZEN,
      VK_DRIVER_ID_MESA_NVK,     VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA,
      VK_DRIVER_ID_MESA_HONEYKRISP,
  };
  return std::ranges::find(MESA_IDS, id) != MESA_IDS.end();
}

// The DriverDetails driver of the Mesa drivers that have bug entries; others are unknown.
DriverDetails::Driver MesaDriverFor(VkDriverId id)
{
  if (id == VK_DRIVER_ID_MESA_RADV)
    return DriverDetails::DRIVER_R600;
  if (id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA)
    return DriverDetails::DRIVER_I965;
  if (id == VK_DRIVER_ID_MESA_NVK)
    return DriverDetails::DRIVER_NOUVEAU;
  if (id == VK_DRIVER_ID_MESA_TURNIP)
    return DriverDetails::DRIVER_FREEDRENO;
  return DriverDetails::DRIVER_UNKNOWN;
}

bool AsBool(VkBool32 value)
{
  return value != VK_FALSE;
}

VkBool32 AsVk(bool value)
{
  return value ? VK_TRUE : VK_FALSE;
}
}  // namespace

VulkanContext::PhysicalDeviceInfo::PhysicalDeviceInfo(VkPhysicalDevice device)
{
  VkPhysicalDeviceProperties2 props2;
  VkPhysicalDeviceProperties& props = props2.properties;
  VkPhysicalDeviceFeatures feats;

  vkGetPhysicalDeviceProperties(device, &props);
  vkGetPhysicalDeviceFeatures(device, &feats);

  // Without vkGetPhysicalDeviceProperties2 nothing past 1.0 can be queried, so treat it as 1.0.
  apiVersion = vkGetPhysicalDeviceProperties2 ? props.apiVersion : VK_API_VERSION_1_0;

  if (apiVersion >= VK_API_VERSION_1_1)
  {
    VkPhysicalDeviceSubgroupProperties subgroup = {};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceVulkan12Properties vk12 = {};
    vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;

    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = nullptr;
    ChainBehind(props2, subgroup);
    const bool has_vk12 = apiVersion >= VK_API_VERSION_1_2;
    if (has_vk12)
      ChainBehind(props2, vk12);

    vkGetPhysicalDeviceProperties2(device, &props2);

    if (has_vk12)
      driverID = vk12.driverID;
    subgroupSize = subgroup.subgroupSize;

    // The subgroup paths in the shaders need elect/invocation id (basic), ballot and
    // min/max (arithmetic), plus shuffle, in fragment shaders.
    constexpr VkSubgroupFeatureFlags needed_ops =
        VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
        VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    const bool ops_ok = (subgroup.supportedOperations & needed_ops) == needed_ops;
    const bool fragment_ok = (subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0;
    shaderSubgroupOperations = ops_ok && fragment_ok;
  }

  std::memcpy(deviceName, props.deviceName, sizeof(deviceName));
  std::memcpy(pipelineCacheUUID, props.pipelineCacheUUID, sizeof(pipelineCacheUUID));
  vendorID = props.vendorID;
  deviceID = props.deviceID;

  const VkPhysicalDeviceLimits& limits = props.limits;
  minUniformBufferOffsetAlignment =
      std::max<VkDeviceSize>(limits.minUniformBufferOffsetAlignment, 1);
  bufferImageGranularity = std::max<VkDeviceSize>(limits.bufferImageGranularity, 1);
  maxTexelBufferElements = limits.maxTexelBufferElements;
  maxImageDimension2D = limits.maxImageDimension2D;
  framebufferColorSampleCounts = limits.framebufferColorSampleCounts;
  framebufferDepthSampleCounts = limits.framebufferDepthSampleCounts;
  pointSizeRange[0] = limits.pointSizeRange[0];
  pointSizeRange[1] = limits.pointSizeRange[1];
  maxSamplerAnisotropy = limits.maxSamplerAnisotropy;

  dualSrcBlend = AsBool(feats.dualSrcBlend);
  geometryShader = AsBool(feats.geometryShader);
  samplerAnisotropy = AsBool(feats.samplerAnisotropy);
  logicOp = AsBool(feats.logicOp);
  fragmentStoresAndAtomics = AsBool(feats.fragmentStoresAndAtomics);
  sampleRateShading = AsBool(feats.sampleRateShading);
  largePoints = AsBool(feats.largePoints);
  shaderStorageImageMultisample = AsBool(feats.shaderStorageImageMultisample);
  shaderTessellationAndGeometryPointSize = AsBool(feats.shaderTessellationAndGeometryPointSize);
  occlusionQueryPrecise = AsBool(feats.occlusionQueryPrecise);
  shaderClipDistance = AsBool(feats.shaderClipDistance);
  depthClamp = AsBool(feats.depthClamp);
  textureCompressionBC = AsBool(feats.textureCompressionBC);
}

VkPhysicalDeviceFeatures VulkanContext::PhysicalDeviceInfo::features() const
{
  // Everything not listed here stays off.
  VkPhysicalDeviceFeatures enabled = {};
  enabled.dualSrcBlend = AsVk(dualSrcBlend);
  enabled.geometryShader = AsVk(geometryShader);
  enabled.samplerAnisotropy = AsVk(samplerAnisotropy);
  enabled.logicOp = AsVk(logicOp);
  enabled.fragmentStoresAndAtomics = AsVk(fragmentStoresAndAtomics);
  enabled.sampleRateShading = AsVk(sampleRateShading);
  enabled.largePoints = AsVk(largePoints);
  enabled.shaderStorageImageMultisample = AsVk(shaderStorageImageMultisample);
  enabled.shaderTessellationAndGeometryPointSize = AsVk(shaderTessellationAndGeometryPointSize);
  enabled.occlusionQueryPrecise = AsVk(occlusionQueryPrecise);
  enabled.shaderClipDistance = AsVk(shaderClipDistance);
  enabled.depthClamp = AsVk(depthClamp);
  enabled.textureCompressionBC = AsVk(textureCompressionBC);
  return enabled;
}

VulkanContext::VulkanContext(VkInstance instance, VkPhysicalDevice physical_device)
    : m_instance(instance), m_physical_device(physical_device), m_info(physical_device)
{
}

VulkanContext::~VulkanContext()
{
  if (m_allocator != VK_NULL_HANDLE)
    vmaDestroyAllocator(m_allocator);
  if (m_device != VK_NULL_HANDLE)
    vkDestroyDevice(m_device, nullptr);
  if (m_messenger != VK_NULL_HANDLE)
    DisableDebugUtils();
  vkDestroyInstance(m_instance, nullptr);
}

bool VulkanContext::CheckValidationLayerAvailablility()
{
  constexpr const char* enumerate_failed = "vkEnumerateInstanceExtensionProperties failed: ";

  std::vector<VkExtensionProperties> extensions;
  if (!QueryInstanceExtensions(nullptr, &extensions, enumerate_failed))
    return false;

  u32 layer_count = 0;
  VkResult res = vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, enumerate_failed);
    return false;
  }
  std::vector<VkLayerProperties> layers(layer_count);
  res = vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
  ASSERT(res == VK_SUCCESS);

  const bool have_layer = HasNamed(layers, KHRONOS_VALIDATION_LAYER);
  bool have_debug_utils = HasNamed(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  // The validation layer can provide debug utils itself when the loader does not.
  if (have_layer && !have_debug_utils)
  {
    if (!QueryInstanceExtensions(KHRONOS_VALIDATION_LAYER, &extensions, enumerate_failed))
      return false;
    have_debug_utils = HasNamed(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  return have_layer && have_debug_utils;
}

VkInstance VulkanContext::CreateVulkanInstance(WindowSystemType wstype, bool enable_debug_utils,
                                               bool enable_validation_layer,
                                               u32* out_vk_api_version)
{
  std::vector<const char*> extensions;
  if (!SelectInstanceExtensions(&extensions, wstype, enable_debug_utils, enable_validation_layer))
    return VK_NULL_HANDLE;

  // The application identity drivers see (and may key profiles on) is kept as it always was.
  VkApplicationInfo app = {};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "Dolphin Emulator";
  app.applicationVersion = VK_MAKE_VERSION(5, 0, 0);
  app.pEngineName = "Dolphin Emulator";
  app.engineVersion = VK_MAKE_VERSION(5, 0, 0);
  app.apiVersion = PickInstanceApiVersion();
  *out_vk_api_version = app.apiVersion;

  VkInstanceCreateInfo create = {};
  create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create.pApplicationInfo = &app;
  create.enabledExtensionCount = static_cast<u32>(extensions.size());
  create.ppEnabledExtensionNames = extensions.data();
  if (enable_validation_layer)
  {
    create.enabledLayerCount = 1;
    create.ppEnabledLayerNames = &KHRONOS_VALIDATION_LAYER;
  }

  VkInstance instance;
  const VkResult res = vkCreateInstance(&create, nullptr, &instance);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateInstance failed: ");
    return VK_NULL_HANDLE;
  }
  return instance;
}

bool VulkanContext::SelectInstanceExtensions(std::vector<const char*>* extension_list,
                                             WindowSystemType wstype, bool enable_debug_utils,
                                             bool validation_layer_enabled)
{
  u32 count = 0;
  VkResult res = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkEnumerateInstanceExtensionProperties failed: ");
    return false;
  }
  if (count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "Vulkan: No extensions supported by instance.");
    return false;
  }
  std::vector<VkExtensionProperties> loader_exts(count);
  res = vkEnumerateInstanceExtensionProperties(nullptr, &count, loader_exts.data());
  ASSERT(res == VK_SUCCESS);

  // Extensions the validation layer brings along also count when the layer is on. A failure
  // here only loses those.
  std::vector<VkExtensionProperties> layer_exts;
  if (validation_layer_enabled)
  {
    u32 layer_count = 0;
    res = vkEnumerateInstanceExtensionProperties(KHRONOS_VALIDATION_LAYER, &layer_count, nullptr);
    if (res == VK_SUCCESS)
    {
      layer_exts.resize(layer_count);
      res = vkEnumerateInstanceExtensionProperties(KHRONOS_VALIDATION_LAYER, &layer_count,
                                                   layer_exts.data());
      ASSERT(res == VK_SUCCESS);
    }
    else
    {
      LOG_VULKAN_ERROR(res, "vkEnumerateInstanceExtensionProperties failed for validation layers: ");
    }
  }

  for (const VkExtensionProperties& ext : loader_exts)
    INFO_LOG_FMT(VIDEO, "Available extension: {}", ext.extensionName);
  for (const VkExtensionProperties& ext : layer_exts)
    INFO_LOG_FMT(VIDEO, "Available extension in validation layer: {}", ext.extensionName);

  const auto enable = [&](const char* name, bool required) {
    if (HasNamed(loader_exts, name) || HasNamed(layer_exts, name))
    {
      INFO_LOG_FMT(VIDEO, "Enabling extension: {}", name);
      extension_list->push_back(name);
      return true;
    }
    if (required)
      ERROR_LOG_FMT(VIDEO, "Vulkan: Missing required extension {}.", name);
    return false;
  };

  const bool windowed = wstype != WindowSystemType::Headless;
  if (windowed && !enable(VK_KHR_SURFACE_EXTENSION_NAME, true))
    return false;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
  if (wstype == WindowSystemType::Windows && !enable(VK_KHR_WIN32_SURFACE_EXTENSION_NAME, true))
    return false;
#endif

  enable(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, false);
  if (windowed)
    enable(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, false);

  if (enable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, false))
    g_backend_info.bSupportsSettingObjectNames = true;
  else if (enable_debug_utils)
    WARN_LOG_FMT(VIDEO, "Vulkan: Debug utils requested, but extension is not available.");

  return true;
}

VulkanContext::GPUList VulkanContext::EnumerateGPUs(VkInstance instance)
{
  u32 count = 0;
  VkResult res = vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (res == VK_SUCCESS)
  {
    GPUList gpus(count);
    res = vkEnumeratePhysicalDevices(instance, &count, gpus.data());
    if (res == VK_SUCCESS)
      return gpus;
  }
  LOG_VULKAN_ERROR(res, "vkEnumeratePhysicalDevices failed: ");
  return {};
}

void VulkanContext::PopulateBackendInfo(BackendInfo* backend_info)
{
  BackendInfo& info = *backend_info;
  info.api_type = APIType::Vulkan;

  // Always available with the Vulkan feature level the backend requires.
  info.bSupportsEarlyZ = true;
  info.bSupportsPrimitiveRestart = true;
  info.bSupportsPaletteConversion = true;
  info.bSupportsClipControl = true;
  info.bSupportsMultithreading = true;
  info.bSupportsComputeShaders = true;
  info.bSupportsGPUTextureDecoding = true;
  info.bSupportsBitfield = true;
  info.bSupportsPartialDepthCopies = true;
  info.bSupportsShaderBinaries = true;
  info.bSupportsDynamicSamplerIndexing = true;
  info.bSupportsPostProcessing = true;
  info.bSupportsBackgroundCompiling = true;
  info.bSupportsCopyToVram = true;
  info.bSupportsReversedDepthRange = true;
  info.bSupportsCoarseDerivatives = true;
  info.bSupportsTextureQueryLevels = true;
  info.bSupportsPartialMultisampleResolve = true;
  info.bSupportsDynamicVertexLoader = true;
  info.bSupportsVSLinePointExpand = true;
  info.bSupportsHDROutput = true;

  // Never available: D3D-only, explicit binding layouts, and driver-side pipeline blobs
  // (pipelines go through a VkPipelineCache instead).
  info.bSupports3DVision = false;
  info.bSupportsBindingLayout = false;
  info.bSupportsPipelineCacheData = false;

  // Decided later from the device features, the extensions and the surface.
  info.bSupportsExclusiveFullscreen = false;
  info.bSupportsDualSourceBlend = false;
  info.bSupportsGeometryShaders = false;
  info.bSupportsGSInstancing = false;
  info.bSupportsBBox = false;
  info.bSupportsFragmentStoresAndAtomics = false;
  info.bSupportsSSAA = false;
  info.bSupportsDepthClamp = false;
  info.bSupportsST3CTextures = false;
  info.bSupportsBPTCTextures = false;
  info.bSupportsLogicOp = false;
  info.bSupportsLargePoints = false;
  info.bSupportsLodBiasInSampler = false;
  info.bSupportsSettingObjectNames = false;
  info.bSupportsUnrestrictedDepthRange = false;
}

void VulkanContext::PopulateBackendInfoAdapters(BackendInfo* backend_info, const GPUList& gpu_list)
{
  backend_info->Adapters.clear();
  for (VkPhysicalDevice gpu : gpu_list)
  {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    backend_info->Adapters.push_back(props.deviceName);
  }
}

void VulkanContext::PopulateBackendInfoFeatures(BackendInfo* backend_info, VkPhysicalDevice,
                                                const PhysicalDeviceInfo& info)
{
  BackendInfo& out = *backend_info;
  out.MaxTextureSize = info.maxImageDimension2D;
  out.bUsesLowerLeftOrigin = false;
  out.bSupportsDualSourceBlend = info.dualSrcBlend;
  out.bSupportsSSAA = info.sampleRateShading;
  out.bSupportsLogicOp = info.logicOp;

  // Geometry shaders write gl_Layer, which needs the point-size feature as well.
  const bool usable_gs = info.geometryShader && info.shaderTessellationAndGeometryPointSize;
  out.bSupportsGeometryShaders = usable_gs;
  out.bSupportsGSInstancing = usable_gs;

  // Bounding box is written from fragment shaders through an SSBO.
  out.bSupportsFragmentStoresAndAtomics = info.fragmentStoresAndAtomics;
  out.bSupportsBBox = info.fragmentStoresAndAtomics;

  // MoltenVK (Metal) has no sampler LOD bias.
  out.bSupportsLodBiasInSampler = info.driverID != VK_DRIVER_ID_MOLTENVK;

  // The depth clamp path also writes clip distances.
  out.bSupportsDepthClamp = info.depthClamp && info.shaderClipDistance;

  // BC1-BC7 cover the S3TC (DXT1/3/5) and BPTC formats.
  out.bSupportsST3CTextures = info.textureCompressionBC;
  out.bSupportsBPTCTextures = info.textureCompressionBC;

  // EFB pokes draw points up to the largest internal resolution (16); without such points they
  // fall back to triangles.
  out.bSupportsLargePoints =
      info.largePoints && info.pointSizeRange[0] <= 1.0f && info.pointSizeRange[1] >= 16;

  // Features that are present but broken on the detected driver.
  static constexpr std::pair<DriverDetails::Bug, bool BackendInfo::*> BROKEN_FEATURES[] = {
      {DriverDetails::BUG_PRIMITIVE_RESTART, &BackendInfo::bSupportsPrimitiveRestart},
      {DriverDetails::BUG_BROKEN_REVERSED_DEPTH_RANGE, &BackendInfo::bSupportsReversedDepthRange},
      {DriverDetails::BUG_BROKEN_DYNAMIC_SAMPLER_INDEXING,
       &BackendInfo::bSupportsDynamicSamplerIndexing},
  };
  for (const auto& [bug, feature] : BROKEN_FEATURES)
  {
    if (DriverDetails::HasBug(bug))
      out.*feature = false;
  }

  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DISCARD_WITH_EARLY_Z))
  {
    PanicAlertFmtT(
        "You are attempting to use the Vulkan backend on an unsupported operating system. "
        "To prevent visual glitches and artifacts, please use the Metal backend or update "
        "to macOS Sonoma 14 or newer.");
  }
}

void VulkanContext::PopulateBackendInfoMultisampleModes(BackendInfo* dest,
                                                        VkPhysicalDevice physical_device,
                                                        const PhysicalDeviceInfo& info)
{
  // MSAA is only offered at counts both EFB formats can be rendered with.
  VkImageFormatProperties color = {};
  vkGetPhysicalDeviceImageFormatProperties(physical_device, VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &color);
  VkImageFormatProperties depth = {};
  vkGetPhysicalDeviceImageFormatProperties(physical_device, VK_FORMAT_D32_SFLOAT,
                                           VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &depth);

  const VkSampleCountFlags usable = info.framebufferColorSampleCounts &
                                    info.framebufferDepthSampleCounts & color.sampleCounts &
                                    depth.sampleCounts;

  dest->AAModes.clear();
  dest->AAModes.emplace_back(1);
  for (u32 samples = 2; samples <= 64; samples *= 2)
  {
    // VK_SAMPLE_COUNT_n_BIT has the value n.
    if (usable & samples)
      dest->AAModes.emplace_back(samples);
  }
}

std::unique_ptr<VulkanContext> VulkanContext::Create(VkInstance instance, VkPhysicalDevice gpu,
                                                     VkSurfaceKHR surface, bool enable_debug_utils,
                                                     bool enable_validation_layer,
                                                     u32 vk_api_version)
{
  auto context = std::make_unique<VulkanContext>(instance, gpu);

  // The bug table decides some of the extensions and features below.
  context->InitDriverDetails();

  if (enable_debug_utils)
    context->EnableDebugUtils();

  if (context->CreateDevice(surface, enable_validation_layer) &&
      context->CreateAllocator(vk_api_version))
  {
    return context;
  }

  // The context's destructor takes the instance down; the surface is ours to free here.
  if (surface != VK_NULL_HANDLE)
    vkDestroySurfaceKHR(instance, surface, nullptr);
  return nullptr;
}

bool VulkanContext::SelectDeviceExtensions(bool enable_surface)
{
  u32 count = 0;
  VkResult res = vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &count, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkEnumerateDeviceExtensionProperties failed: ");
    return false;
  }
  if (count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "Vulkan: No extensions supported by device.");
    return false;
  }
  std::vector<VkExtensionProperties> available(count);
  res = vkEnumerateDeviceExtensionProperties(m_physical_device, nullptr, &count, available.data());
  ASSERT(res == VK_SUCCESS);

  for (const VkExtensionProperties& ext : available)
    INFO_LOG_FMT(VIDEO, "Available extension: {}", ext.extensionName);

  const auto enable = [&](const char* name, bool required) {
    if (HasNamed(available, name))
    {
      INFO_LOG_FMT(VIDEO, "Enabling extension: {}", name);
      m_enabled_device_extensions.emplace_back(name);
      return true;
    }
    if (required)
      ERROR_LOG_FMT(VIDEO, "Vulkan: Missing required extension {}.", name);
    return false;
  };

  if (enable_surface && !enable(VK_KHR_SWAPCHAIN_EXTENSION_NAME, true))
    return false;

#ifdef SUPPORTS_VULKAN_EXCLUSIVE_FULLSCREEN
  // Reported as required in the log when missing, but the device is still created without it.
  if (enable(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME, true))
    INFO_LOG_FMT(VIDEO, "Using VK_EXT_full_screen_exclusive for exclusive fullscreen.");
#endif

  enable(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, false);
  enable(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, false);

  // VK_EXT_depth_range_unrestricted changes depth behaviour as soon as it is enabled, so it is
  // only enabled together with (and after) depth clamp control.
  if (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DEPTH_CLAMP_CONTROL))
  {
    bool unrestricted = enable(VK_EXT_DEPTH_CLAMP_CONTROL_EXTENSION_NAME, false);
    if (unrestricted)
      unrestricted = enable(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME, false);
    g_backend_info.bSupportsUnrestrictedDepthRange = unrestricted;
  }

  return true;
}

void VulkanContext::WarnMissingDeviceFeatures()
{
  if (!m_info.largePoints)
    WARN_LOG_FMT(VIDEO, "Vulkan: Missing large points feature. CPU EFB writes will be slower.");
  if (!m_info.occlusionQueryPrecise)
  {
    WARN_LOG_FMT(VIDEO,
                 "Vulkan: Missing precise occlusion queries. Perf queries will be inaccurate.");
  }
}

bool VulkanContext::FindQueueFamilies(VkSurfaceKHR surface)
{
  u32 family_count;
  vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count, nullptr);
  if (family_count == 0)
  {
    ERROR_LOG_FMT(VIDEO, "No queue families found on specified vulkan physical device.");
    return false;
  }
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count, families.data());
  INFO_LOG_FMT(VIDEO, "{} vulkan queue families", family_count);

  // "Not found" is the family count. The scan stops at the first graphics family when there is
  // no surface, or at the first family that does both graphics and present; otherwise the last
  // match of each kind wins.
  m_graphics_family = family_count;
  m_present_family = family_count;
  for (u32 family = 0; family < family_count; family++)
  {
    const bool graphics = (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
    if (graphics)
      m_graphics_family = family;

    if (surface == VK_NULL_HANDLE)
    {
      if (graphics)
        break;
      continue;
    }

    VkBool32 can_present;
    const VkResult res =
        vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, family, surface, &can_present);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceSupportKHR failed: ");
      return false;
    }
    if (can_present)
      m_present_family = family;
    if (graphics && can_present)
      break;
  }

  if (m_graphics_family == family_count)
  {
    ERROR_LOG_FMT(VIDEO, "Vulkan: Failed to find an acceptable graphics queue.");
    return false;
  }
  if (surface != VK_NULL_HANDLE && m_present_family == family_count)
  {
    ERROR_LOG_FMT(VIDEO, "Vulkan: Failed to find an acceptable present queue.");
    return false;
  }
  return true;
}

bool VulkanContext::CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer)
{
  if (!FindQueueFamilies(surface))
    return false;

  // One queue from the graphics family, and one from the present family when that differs.
  // Without a surface there is no present family (m_present_family holds "not found").
  static constexpr float priority = 1.0f;
  std::array<VkDeviceQueueCreateInfo, 2> queues = {};
  for (VkDeviceQueueCreateInfo& queue : queues)
  {
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
  }
  queues[0].queueFamilyIndex = m_graphics_family;
  queues[1].queueFamilyIndex = m_present_family;
  const bool separate_present =
      surface != VK_NULL_HANDLE && m_present_family != m_graphics_family;

  if (!SelectDeviceExtensions(surface != VK_NULL_HANDLE))
    return false;

  std::vector<const char*> extension_names;
  extension_names.reserve(m_enabled_device_extensions.size());
  for (const std::string& name : m_enabled_device_extensions)
    extension_names.push_back(name.c_str());

  WarnMissingDeviceFeatures();
  const VkPhysicalDeviceFeatures features = m_info.features();

  VkDeviceCreateInfo create = {};
  create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  create.queueCreateInfoCount = separate_present ? 2 : 1;
  create.pQueueCreateInfos = queues.data();
  create.enabledExtensionCount = static_cast<u32>(extension_names.size());
  create.ppEnabledExtensionNames = extension_names.data();
  create.pEnabledFeatures = &features;
  if (enable_validation_layer)
  {
    create.enabledLayerCount = 1;
    create.ppEnabledLayerNames = &KHRONOS_VALIDATION_LAYER;
  }

  const VkResult res = vkCreateDevice(m_physical_device, &create, nullptr, &m_device);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateDevice failed: ");
    return false;
  }

  // Device-level entry points can only be fetched once the device exists.
  if (!LoadVulkanDeviceFunctions(m_device))
    return false;

  vkGetDeviceQueue(m_device, m_graphics_family, 0, &m_graphics_queue);
  if (surface != VK_NULL_HANDLE)
    vkGetDeviceQueue(m_device, m_present_family, 0, &m_present_queue);
  return true;
}

bool VulkanContext::CreateAllocator(u32 vk_api_version)
{
  VmaAllocatorCreateInfo create = {};
  create.flags = VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT;
  if (SupportsDeviceExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
    create.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  create.physicalDevice = m_physical_device;
  create.device = m_device;
  create.instance = m_instance;
  create.vulkanApiVersion = vk_api_version;
  // Large heaps are carved into 64 MiB blocks.
  create.preferredLargeHeapBlockSize = 64 << 20;

  const VkResult res = vmaCreateAllocator(&create, &m_allocator);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateAllocator failed: ");
    return false;
  }
  return true;
}

bool VulkanContext::EnableDebugUtils()
{
  if (m_messenger != VK_NULL_HANDLE)
    return true;

  // All three entry points come from VK_EXT_debug_utils and may be absent.
  if (!vkCreateDebugUtilsMessengerEXT || !vkDestroyDebugUtilsMessengerEXT ||
      !vkSubmitDebugUtilsMessageEXT)
  {
    return false;
  }

  VkDebugUtilsMessengerCreateInfoEXT create = {};
  create.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
  create.messageSeverity =
      VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
      VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
  create.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  create.pfnUserCallback = ForwardDebugMessage;

  const VkResult res = vkCreateDebugUtilsMessengerEXT(m_instance, &create, nullptr, &m_messenger);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateDebugUtilsMessengerEXT failed: ");
    return false;
  }
  return true;
}

void VulkanContext::DisableDebugUtils()
{
  if (m_messenger == VK_NULL_HANDLE)
    return;
  vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
  m_messenger = VK_NULL_HANDLE;
}

bool VulkanContext::SupportsDeviceExtension(const char* name) const
{
  return std::ranges::find(m_enabled_device_extensions, std::string_view{name}) !=
         m_enabled_device_extensions.end();
}

void VulkanContext::InitDriverDetails()
{
  // Vulkan has no vendor-neutral way to tell drivers apart before 1.2's VkDriverId, so the
  // PCI vendor ID and the device name (as listed on vulkan.gpuinfo.org) are used as well.
  std::string name = m_info.deviceName;
  const u32 vendor_id = m_info.vendorID;
  const VkDriverId driver_id = m_info.driverID;  // zero before Vulkan 1.2

  const auto named = [&name](std::string_view part) { return name.contains(part); };

  DriverDetails::Vendor vendor = DriverDetails::VENDOR_UNKNOWN;
  DriverDetails::Driver driver = DriverDetails::DRIVER_UNKNOWN;
  if (IsMesaDriverId(driver_id))
  {
    vendor = DriverDetails::VENDOR_MESA;
    driver = MesaDriverFor(driver_id);
  }
  else if (vendor_id == 0x10DE)
  {
    // NVIDIA's own driver; the device name does not say "NVIDIA".
    vendor = DriverDetails::VENDOR_NVIDIA;
    driver = DriverDetails::DRIVER_NVIDIA;
  }
  else if (vendor_id == 0x1002 || vendor_id == 0x1022 || named("AMD"))
  {
    // RADV always puts its name in the device string; anything else is AMD's driver.
    if (named("RADV"))
    {
      vendor = DriverDetails::VENDOR_MESA;
      driver = DriverDetails::DRIVER_R600;
    }
    else
    {
      vendor = DriverDetails::VENDOR_ATI;
      driver = DriverDetails::DRIVER_ATI;
    }
  }
  else if (vendor_id == 0x8086 || vendor_id == 0x8087 || named("Intel"))
  {
    // On Windows an Intel GPU runs Intel's own driver.
    vendor = DriverDetails::VENDOR_INTEL;
    driver = DriverDetails::DRIVER_INTEL;
  }
  else if (vendor_id == 0x5143 || named("Adreno"))
  {
    vendor = DriverDetails::VENDOR_QUALCOMM;
    driver = DriverDetails::DRIVER_QUALCOMM;
  }
  else if (vendor_id == 0x13B6 || named("Mali"))
  {
    vendor = DriverDetails::VENDOR_ARM;
    driver = DriverDetails::DRIVER_ARM;
  }
  else if (vendor_id == 0x1010 || named("PowerVR"))
  {
    vendor = DriverDetails::VENDOR_IMGTEC;
    driver = DriverDetails::DRIVER_IMGTEC;
  }
  else if (named("Apple"))
  {
    vendor = DriverDetails::VENDOR_APPLE;
    driver = DriverDetails::DRIVER_PORTABILITY;
  }
  else
  {
    WARN_LOG_FMT(VIDEO, "Unknown Vulkan driver vendor, please report it to us.");
    WARN_LOG_FMT(VIDEO, "Vendor ID: {:#X}, Device Name: {}", vendor_id, name);
  }

  // MoltenVK runs on Metal and has none of the native drivers' bugs.
  if (driver_id == VK_DRIVER_ID_MOLTENVK)
    driver = DriverDetails::DRIVER_PORTABILITY;

  DriverDetails::Init(DriverDetails::API_VULKAN, vendor, driver,
                      static_cast<double>(m_info.driverVersion), DriverDetails::Family::UNKNOWN,
                      std::move(name));
}

bool VulkanContext::SupportsExclusiveFullscreen(const WindowSystemInfo& wsi, VkSurfaceKHR surface)
{
#ifdef SUPPORTS_VULKAN_EXCLUSIVE_FULLSCREEN
  if (surface == VK_NULL_HANDLE || !vkGetPhysicalDeviceSurfaceCapabilities2KHR ||
      !SupportsDeviceExtension(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME))
  {
    return false;
  }

  VkSurfaceFullScreenExclusiveWin32InfoEXT monitor = GetPlatformExclusiveFullscreenInfo(wsi);
  VkPhysicalDeviceSurfaceInfo2KHR query = {};
  query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
  query.pNext = &monitor;
  query.surface = surface;

  VkSurfaceCapabilitiesFullScreenExclusiveEXT exclusive = {};
  exclusive.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT;
  exclusive.fullScreenExclusiveSupported = VK_TRUE;
  VkSurfaceCapabilities2KHR caps = {};
  caps.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
  caps.pNext = &exclusive;

  const VkResult res = vkGetPhysicalDeviceSurfaceCapabilities2KHR(m_physical_device, &query, &caps);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceCapabilities2KHR failed:");
    return false;
  }
  return exclusive.fullScreenExclusiveSupported != VK_FALSE;
#else
  static_cast<void>(wsi);
  static_cast<void>(surface);
  return false;
#endif
}

#ifdef WIN32
VkSurfaceFullScreenExclusiveWin32InfoEXT
VulkanContext::GetPlatformExclusiveFullscreenInfo(const WindowSystemInfo& wsi)
{
  VkSurfaceFullScreenExclusiveWin32InfoEXT monitor = {};
  monitor.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
  monitor.hmonitor =
      MonitorFromWindow(static_cast<HWND>(wsi.render_surface), MONITOR_DEFAULTTOPRIMARY);
  return monitor;
}
#endif

}  // namespace Vulkan
