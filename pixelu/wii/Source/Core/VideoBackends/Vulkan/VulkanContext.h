// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/WindowSystemInfo.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
// Owns the VkInstance, the chosen physical device, the logical device with its queues and the
// memory allocator. Also answers "what can this GPU do" for the backend info.
class VulkanContext
{
public:
  // The properties and features of a physical device the backend cares about.
  struct PhysicalDeviceInfo
  {
    PhysicalDeviceInfo(const PhysicalDeviceInfo&) = default;
    explicit PhysicalDeviceInfo(VkPhysicalDevice device);

    // The feature set to enable on the logical device.
    VkPhysicalDeviceFeatures features() const;

    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    u8 pipelineCacheUUID[VK_UUID_SIZE];
    u32 apiVersion;
    u32 driverVersion;
    u32 vendorID;
    u32 deviceID;
    VkDeviceSize minUniformBufferOffsetAlignment;
    VkDeviceSize bufferImageGranularity;
    u32 maxTexelBufferElements;
    u32 maxImageDimension2D;
    VkSampleCountFlags framebufferColorSampleCounts;
    VkSampleCountFlags framebufferDepthSampleCounts;
    float pointSizeRange[2];
    float maxSamplerAnisotropy;
    u32 subgroupSize = 1;
    VkDriverId driverID = static_cast<VkDriverId>(0);
    bool dualSrcBlend;
    bool geometryShader;
    bool samplerAnisotropy;
    bool logicOp;
    bool fragmentStoresAndAtomics;
    bool sampleRateShading;
    bool largePoints;
    bool shaderStorageImageMultisample;
    bool shaderTessellationAndGeometryPointSize;
    bool occlusionQueryPrecise;
    bool shaderClipDistance;
    bool depthClamp;
    bool textureCompressionBC;
    bool shaderSubgroupOperations = false;
  };

  using GPUList = std::vector<VkPhysicalDevice>;

  VulkanContext(VkInstance instance, VkPhysicalDevice physical_device);
  ~VulkanContext();

  // True when both VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils can be enabled.
  static bool CheckValidationLayerAvailablility();

  // Creates an instance with the surface and debug extensions this window system needs.
  // Returns VK_NULL_HANDLE on failure; the API version asked for goes to out_vk_api_version.
  static VkInstance CreateVulkanInstance(WindowSystemType wstype, bool enable_debug_utils,
                                         bool enable_validation_layer, u32* out_vk_api_version);

  // The physical devices of an instance, in driver order (empty on failure).
  static GPUList EnumerateGPUs(VkInstance instance);

  // Backend info, in four steps so it can be filled without creating a device: the fixed
  // capabilities, the adapter names, the per-device features and the MSAA modes.
  static void PopulateBackendInfo(BackendInfo* backend_info);
  static void PopulateBackendInfoAdapters(BackendInfo* backend_info, const GPUList& gpu_list);
  static void PopulateBackendInfoFeatures(BackendInfo* backend_info, VkPhysicalDevice gpu,
                                          const PhysicalDeviceInfo& info);
  static void PopulateBackendInfoMultisampleModes(BackendInfo* backend_info, VkPhysicalDevice gpu,
                                                  const PhysicalDeviceInfo& info);

  // Creates the device on `gpu`. Takes over the instance (and the surface on failure).
  static std::unique_ptr<VulkanContext> Create(VkInstance instance, VkPhysicalDevice gpu,
                                               VkSurfaceKHR surface, bool enable_debug_utils,
                                               bool enable_validation_layer, u32 api_version);

  // Installs or removes the debug messenger that forwards driver messages to the log.
  bool EnableDebugUtils();
  void DisableDebugUtils();

  VkInstance GetVulkanInstance() const { return m_instance; }
  VkPhysicalDevice GetPhysicalDevice() const { return m_physical_device; }
  VkDevice GetDevice() const { return m_device; }
  VkQueue GetGraphicsQueue() const { return m_graphics_queue; }
  u32 GetGraphicsQueueFamilyIndex() const { return m_graphics_family; }
  VkQueue GetPresentQueue() const { return m_present_queue; }
  u32 GetPresentQueueFamilyIndex() const { return m_present_family; }
  const VkQueueFamilyProperties& GetGraphicsQueueProperties() const
  {
    return m_graphics_family_properties;
  }
  const PhysicalDeviceInfo& GetDeviceInfo() const { return m_info; }

  bool SupportsAnisotropicFiltering() const { return m_info.samplerAnisotropy; }
  bool SupportsPreciseOcclusionQueries() const { return m_info.occlusionQueryPrecise; }
  u32 GetShaderSubgroupSize() const { return m_info.subgroupSize; }
  bool SupportsShaderSubgroupOperations() const { return m_info.shaderSubgroupOperations; }

  VkDeviceSize GetUniformBufferAlignment() const { return m_info.minUniformBufferOffsetAlignment; }
  VkDeviceSize GetBufferImageGranularity() const { return m_info.bufferImageGranularity; }
  float GetMaxSamplerAnisotropy() const { return m_info.maxSamplerAnisotropy; }

  // Whether `name` was enabled on the device.
  bool SupportsDeviceExtension(const char* name) const;

  // Whether exclusive fullscreen can be used with this surface.
  bool SupportsExclusiveFullscreen(const WindowSystemInfo& wsi, VkSurfaceKHR surface);

  VmaAllocator GetMemoryAllocator() const { return m_allocator; }

#ifdef WIN32
  // The monitor description chained into exclusive-fullscreen queries and swap chains.
  VkSurfaceFullScreenExclusiveWin32InfoEXT
  GetPlatformExclusiveFullscreenInfo(const WindowSystemInfo& wsi);
#endif

private:
  static bool SelectInstanceExtensions(std::vector<const char*>* extension_list,
                                       WindowSystemType wstype, bool enable_debug_utils,
                                       bool validation_layer_enabled);
  bool SelectDeviceExtensions(bool enable_surface);
  bool FindQueueFamilies(VkSurfaceKHR surface);
  void WarnMissingDeviceFeatures();
  bool CreateDevice(VkSurfaceKHR surface, bool enable_validation_layer);
  void InitDriverDetails();
  bool CreateAllocator(u32 vk_api_version);

  VkInstance m_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VmaAllocator m_allocator = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;

  u32 m_graphics_family = 0;
  u32 m_present_family = 0;
  VkQueue m_graphics_queue = VK_NULL_HANDLE;
  VkQueue m_present_queue = VK_NULL_HANDLE;
  VkQueueFamilyProperties m_graphics_family_properties = {};

  PhysicalDeviceInfo m_info;
  std::vector<std::string> m_enabled_device_extensions;
};

extern std::unique_ptr<VulkanContext> g_vulkan_context;

}  // namespace Vulkan
