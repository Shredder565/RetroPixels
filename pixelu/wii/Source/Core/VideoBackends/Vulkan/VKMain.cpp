// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VideoBackend.h"

#include <memory>
#include <utility>

#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKBoundingBox.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VKPerfQuery.h"
#include "VideoBackends/Vulkan/VKSwapChain.h"
#include "VideoBackends/Vulkan/VKVertexManager.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
namespace
{
// The configured adapter, or the first one when the index is past the end of the list.
size_t PickAdapter(const VulkanContext::GPUList& gpus, bool warn)
{
  const size_t wanted = static_cast<size_t>(g_Config.iAdapter);
  if (wanted < gpus.size())
    return wanted;
  if (warn)
    WARN_LOG_FMT(VIDEO, "Vulkan adapter index out of range, selecting first adapter.");
  return 0;
}

// Debug utils are wanted with validation, or when Host GPU errors are being logged. The choice
// is made once, at instance creation: ticking the log category later needs a restart.
bool WantDebugUtils(bool validation)
{
  if (validation)
    return true;
  return Common::Log::LogManager::GetInstance()->IsEnabled(Common::Log::LogType::HOST_GPU,
                                                           Common::Log::LogLevel::LERROR);
}

// Drops an instance that never made it into a context, then the library.
void AbandonInstance(VkInstance instance)
{
  vkDestroyInstance(instance, nullptr);
  UnloadVulkanLibrary();
}
}  // namespace

void VideoBackend::InitBackendInfo(const WindowSystemInfo&)
{
  VulkanContext::PopulateBackendInfo(&g_backend_info);

  if (!LoadVulkanLibrary())
  {
    PanicAlertFmt("Failed to load Vulkan library.");
    return;
  }

  // A throwaway headless instance is enough to list the adapters and their features.
  u32 api_version = 0;
  const VkInstance probe =
      VulkanContext::CreateVulkanInstance(WindowSystemType::Headless, false, false, &api_version);
  if (!probe)
  {
    PanicAlertFmt("Failed to create Vulkan instance.");
    UnloadVulkanLibrary();
    return;
  }

  if (LoadVulkanInstanceFunctions(probe))
  {
    const VulkanContext::GPUList gpus = VulkanContext::EnumerateGPUs(probe);
    VulkanContext::PopulateBackendInfoAdapters(&g_backend_info, gpus);
    if (!gpus.empty())
    {
      const VkPhysicalDevice gpu = gpus[PickAdapter(gpus, false)];
      const VulkanContext::PhysicalDeviceInfo info(gpu);
      VulkanContext::PopulateBackendInfoFeatures(&g_backend_info, gpu, info);
      VulkanContext::PopulateBackendInfoMultisampleModes(&g_backend_info, gpu, info);
    }
  }

  AbandonInstance(probe);
}

bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
  if (!LoadVulkanLibrary())
  {
    PanicAlertFmt("Failed to load Vulkan library.");
    return false;
  }

  bool validation = g_Config.bEnableValidationLayer;
  if (validation && !VulkanContext::CheckValidationLayerAvailablility())
  {
    WARN_LOG_FMT(VIDEO, "Validation layer requested but not available, disabling.");
    validation = false;
  }

  // The instance comes first: the surface and the device list both need it.
  const bool has_window = wsi.type != WindowSystemType::Headless;
  const bool debug_utils = WantDebugUtils(validation);
  u32 api_version = 0;
  const VkInstance instance =
      VulkanContext::CreateVulkanInstance(wsi.type, debug_utils, validation, &api_version);
  if (instance == VK_NULL_HANDLE)
  {
    PanicAlertFmt("Failed to create Vulkan instance.");
    UnloadVulkanLibrary();
    return false;
  }

  if (!LoadVulkanInstanceFunctions(instance))
  {
    PanicAlertFmt("Failed to load Vulkan instance functions.");
    AbandonInstance(instance);
    return false;
  }

  const VulkanContext::GPUList gpus = VulkanContext::EnumerateGPUs(instance);
  if (gpus.empty())
  {
    PanicAlertFmt("No Vulkan physical devices available.");
    AbandonInstance(instance);
    return false;
  }

  VulkanContext::PopulateBackendInfo(&g_backend_info);
  VulkanContext::PopulateBackendInfoAdapters(&g_backend_info, gpus);

  // Device creation looks at the surface (present support), so make it now.
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (has_window)
  {
    surface = SwapChain::CreateVulkanSurface(instance, wsi);
    if (surface == VK_NULL_HANDLE)
    {
      PanicAlertFmt("Failed to create Vulkan surface.");
      AbandonInstance(instance);
      return false;
    }
  }

  // The config has not been validated yet, so the adapter index is checked here. From now on
  // the context owns the instance and the surface.
  g_vulkan_context = VulkanContext::Create(instance, gpus[PickAdapter(gpus, true)], surface,
                                           debug_utils, validation, api_version);
  if (!g_vulkan_context)
  {
    PanicAlertFmt("Failed to create Vulkan device");
    UnloadVulkanLibrary();
    return false;
  }

  // The context already holds the device's properties; no need to query them again.
  const VkPhysicalDevice gpu = g_vulkan_context->GetPhysicalDevice();
  VulkanContext::PopulateBackendInfoFeatures(&g_backend_info, gpu,
                                             g_vulkan_context->GetDeviceInfo());
  VulkanContext::PopulateBackendInfoMultisampleModes(&g_backend_info, gpu,
                                                     g_vulkan_context->GetDeviceInfo());
  g_backend_info.bSupportsExclusiveFullscreen =
      has_window && g_vulkan_context->SupportsExclusiveFullscreen(wsi, surface);

  UpdateActiveConfig();

  g_object_cache = std::make_unique<ObjectCache>();
  if (!g_object_cache->Initialize())
  {
    PanicAlertFmt("Failed to initialize Vulkan object cache.");
    Shutdown();
    return false;
  }

  // The swap chain is made before the renderer so auto-scaling sees the right window size.
  std::unique_ptr<SwapChain> swap_chain;
  if (surface != VK_NULL_HANDLE)
  {
    swap_chain = SwapChain::Create(wsi, surface, g_ActiveConfig.bVSyncActive);
    if (!swap_chain)
    {
      PanicAlertFmt("Failed to create Vulkan swap chain.");
      Shutdown();
      return false;
    }
  }

  g_command_buffer_mgr = std::make_unique<CommandBufferManager>(g_Config.bBackendMultithreading);
  const size_t image_count = swap_chain ? swap_chain->GetSwapChainImageCount() : 0;
  if (!g_command_buffer_mgr->Initialize(image_count))
  {
    PanicAlertFmt("Failed to create Vulkan command buffers");
    Shutdown();
    return false;
  }

  if (!StateTracker::CreateInstance())
  {
    PanicAlertFmt("Failed to create state tracker");
    Shutdown();
    return false;
  }

  // Built in this order on purpose: each one may use the ones before it.
  auto gfx = std::make_unique<VKGfx>(std::move(swap_chain), wsi.render_surface_scale);
  auto vertex_manager = std::make_unique<VertexManager>();
  auto perf_query = std::make_unique<PerfQuery>();
  auto bbox = std::make_unique<VKBoundingBox>();
  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bbox));
}

void VideoBackend::Shutdown()
{
  // Nothing may still be running on the GPU while objects are torn down.
  if (g_vulkan_context)
    vkDeviceWaitIdle(g_vulkan_context->GetDevice());

  if (g_object_cache)
    g_object_cache->Shutdown();

  ShutdownShared();

  g_object_cache.reset();
  StateTracker::DestroyInstance();
  g_command_buffer_mgr.reset();
  g_vulkan_context.reset();
  UnloadVulkanLibrary();
}

void VideoBackend::PrepareWindow(WindowSystemInfo&)
{
  // Only macOS needed window preparation (a Metal layer); the supported platforms need none.
}
}  // namespace Vulkan
