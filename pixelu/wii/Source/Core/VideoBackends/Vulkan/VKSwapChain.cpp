// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKSwapChain.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include "Common/Assert.h"
#include "Common/Contains.h"
#include "Common/DynamicLibrary.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/Present.h"

namespace Vulkan
{
namespace
{
VkPhysicalDevice Gpu()
{
  return g_vulkan_context->GetPhysicalDevice();
}

VkDevice Device()
{
  return g_vulkan_context->GetDevice();
}

// The usual two-call enumeration. Fails (logging `error`) on an error or an empty list.
template <typename T, typename Query>
bool Enumerate(std::vector<T>* out, const char* error, Query query)
{
  u32 count = 0;
  VkResult res = query(&count, static_cast<T*>(nullptr));
  if (res != VK_SUCCESS || count == 0)
  {
    LOG_VULKAN_ERROR(res, error);
    return false;
  }
  out->resize(count);
  res = query(&count, out->data());
  ASSERT(res == VK_SUCCESS);
  return true;
}

AbstractTextureFormat TextureFormatOf(VkFormat format)
{
  switch (format)
  {
  case VK_FORMAT_R8G8B8A8_UNORM:
    return AbstractTextureFormat::RGBA8;
  case VK_FORMAT_B8G8R8A8_UNORM:
    return AbstractTextureFormat::BGRA8;
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    return AbstractTextureFormat::RGB10_A2;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return AbstractTextureFormat::RGBA16F;
  default:
    return AbstractTextureFormat::Undefined;
  }
}
}  // namespace

SwapChain::SwapChain(const WindowSystemInfo& wsi, VkSurfaceKHR surface, bool vsync)
    : m_wsi(wsi), m_surface(surface), m_vsync(vsync),
      m_fullscreen_supported(g_vulkan_context->SupportsExclusiveFullscreen(wsi, surface))
{
}

SwapChain::~SwapChain()
{
  ReleaseImages();
  ReleaseSwapChain();
  ReleaseSurface();
}

VkSurfaceKHR SwapChain::CreateVulkanSurface(VkInstance instance, const WindowSystemInfo& wsi)
{
#if defined(VK_USE_PLATFORM_WIN32_KHR)
  if (wsi.type == WindowSystemType::Windows)
  {
    VkWin32SurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = nullptr;
    info.hwnd = static_cast<HWND>(wsi.render_surface);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult res = vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateWin32SurfaceKHR failed: ");
      return VK_NULL_HANDLE;
    }
    return surface;
  }
#elif defined(RP_VULKAN_LINUX_SURFACES)
  if (wsi.type == WindowSystemType::X11)
    return CreateX11Surface(instance, wsi);
  if (wsi.type == WindowSystemType::Wayland)
    return CreateWaylandSurface(instance, wsi);
#else
  static_cast<void>(instance);
  static_cast<void>(wsi);
#endif
  return VK_NULL_HANDLE;
}

#if defined(RP_VULKAN_LINUX_SURFACES)
// On X11 the frontend hands over its Xlib Display* (display_connection) and the X window id of
// the render widget (render_surface, an XID stored in a pointer).
VkSurfaceKHR SwapChain::CreateX11Surface(VkInstance instance, const WindowSystemInfo& wsi)
{
  const auto window_id = reinterpret_cast<uintptr_t>(wsi.render_surface);
  if (wsi.display_connection == nullptr || window_id == 0)
  {
    ERROR_LOG_FMT(VIDEO, "Vulkan: X11 surface needs a display connection and a window");
    return VK_NULL_HANDLE;
  }

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (VulkanContext::IsInstanceExtensionEnabled(VK_KHR_XLIB_SURFACE_EXTENSION_NAME) &&
      vkCreateXlibSurfaceKHR != nullptr)
  {
    VkXlibSurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy = static_cast<RpXlibDisplay*>(wsi.display_connection);
    info.window = static_cast<RpXlibWindow>(window_id);
    const VkResult res = vkCreateXlibSurfaceKHR(instance, &info, nullptr, &surface);
    if (res == VK_SUCCESS)
    {
      INFO_LOG_FMT(VIDEO, "Vulkan: Xlib surface for window {:#x}", window_id);
      return surface;
    }
    LOG_VULKAN_ERROR(res, "vkCreateXlibSurfaceKHR failed: ");
  }

  if (VulkanContext::IsInstanceExtensionEnabled(VK_KHR_XCB_SURFACE_EXTENSION_NAME) &&
      vkCreateXcbSurfaceKHR != nullptr)
  {
    // The XCB connection an Xlib display runs on comes from libX11-xcb, which every Xlib
    // program that uses XCB already has loaded; it is looked up at run time so the backend does
    // not link against X11 at all.
    using GetXcbConnectionFn = xcb_connection_t* (*)(void* display);
    static Common::DynamicLibrary x11_xcb(
        Common::DynamicLibrary::GetVersionedFilename("X11-xcb", 1).c_str());
    const auto get_xcb_connection =
        x11_xcb.IsOpen() ?
            reinterpret_cast<GetXcbConnectionFn>(x11_xcb.GetSymbolAddress("XGetXCBConnection")) :
            nullptr;

    xcb_connection_t* const connection =
        get_xcb_connection != nullptr ? get_xcb_connection(wsi.display_connection) : nullptr;
    if (connection == nullptr)
    {
      ERROR_LOG_FMT(VIDEO, "Vulkan: no XCB connection for the X11 display (libX11-xcb.so.1)");
      return VK_NULL_HANDLE;
    }

    VkXcbSurfaceCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    info.connection = connection;
    info.window = static_cast<xcb_window_t>(window_id);
    const VkResult res = vkCreateXcbSurfaceKHR(instance, &info, nullptr, &surface);
    if (res == VK_SUCCESS)
    {
      INFO_LOG_FMT(VIDEO, "Vulkan: XCB surface for window {:#x}", window_id);
      return surface;
    }
    LOG_VULKAN_ERROR(res, "vkCreateXcbSurfaceKHR failed: ");
  }

  return VK_NULL_HANDLE;
}

// On Wayland display_connection is the wl_display* and render_surface the wl_surface*.
VkSurfaceKHR SwapChain::CreateWaylandSurface(VkInstance instance, const WindowSystemInfo& wsi)
{
  if (wsi.display_connection == nullptr || wsi.render_surface == nullptr ||
      vkCreateWaylandSurfaceKHR == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Vulkan: Wayland surface needs a display, a surface and {}",
                  VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
    return VK_NULL_HANDLE;
  }

  VkWaylandSurfaceCreateInfoKHR info = {};
  info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
  info.display = static_cast<wl_display*>(wsi.display_connection);
  info.surface = static_cast<wl_surface*>(wsi.render_surface);

  VkSurfaceKHR surface = VK_NULL_HANDLE;
  const VkResult res = vkCreateWaylandSurfaceKHR(instance, &info, nullptr, &surface);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateWaylandSurfaceKHR failed: ");
    return VK_NULL_HANDLE;
  }
  return surface;
}
#endif

std::unique_ptr<SwapChain> SwapChain::Create(const WindowSystemInfo& wsi, VkSurfaceKHR surface,
                                             bool vsync)
{
  auto swap_chain = std::make_unique<SwapChain>(wsi, surface, vsync);
  return swap_chain->Rebuild() ? std::move(swap_chain) : nullptr;
}

bool SwapChain::Rebuild()
{
  return BuildSwapChain() && WrapImages();
}

bool SwapChain::ChooseSurfaceFormat()
{
  std::vector<VkSurfaceFormatKHR> formats;
  if (!Enumerate(&formats, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: ",
                 [this](u32* count, VkSurfaceFormatKHR* data) {
                   return vkGetPhysicalDeviceSurfaceFormatsKHR(Gpu(), m_surface, count, data);
                 }))
  {
    return false;
  }

  // A lone UNDEFINED entry means any format will do.
  if (formats[0].format == VK_FORMAT_UNDEFINED)
  {
    m_surface_format = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    return true;
  }

  // Candidates by preference: scRGB half-float (HDR only), 10-bit, RGBA8, BGRA8. sRGB formats
  // count as their UNORM twins: presenting must not apply a second gamma curve. The last match
  // of each kind wins.
  enum Candidate
  {
    HDR_FP16,
    RGB10_A2,
    RGBA8,
    BGRA8,
    CANDIDATE_COUNT
  };
  std::array<const VkSurfaceFormatKHR*, CANDIDATE_COUNT> found = {};
  for (const VkSurfaceFormatKHR& entry : formats)
  {
    const VkFormat linear = VKTexture::GetLinearFormat(entry.format);
    if (entry.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
      if (linear == VK_FORMAT_R8G8B8A8_UNORM)
        found[RGBA8] = &entry;
      else if (linear == VK_FORMAT_B8G8R8A8_UNORM)
        found[BGRA8] = &entry;
      else if (linear == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
        found[RGB10_A2] = &entry;
    }
    else if (entry.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT &&
             linear == VK_FORMAT_R16G16B16A16_SFLOAT)
    {
      found[HDR_FP16] = &entry;
    }
  }

  // g_Config, not g_ActiveConfig: this can run before the active config is refreshed.
  if (!g_Config.bHDR)
    found[HDR_FP16] = nullptr;
  const auto best = std::find_if(found.begin(), found.end(),
                                 [](const VkSurfaceFormatKHR* entry) { return entry != nullptr; });
  if (best == found.end())
  {
    PanicAlertFmt("Failed to find a suitable format for swap chain buffers.");
    return false;
  }

  const VkFormat linear = VKTexture::GetLinearFormat((*best)->format);
  m_texture_format = TextureFormatOf(linear);
  m_surface_format = {linear, (*best)->colorSpace};
  return true;
}

bool SwapChain::ChoosePresentMode()
{
  std::vector<VkPresentModeKHR> modes;
  // (The error text names the format query, as it always has.)
  if (!Enumerate(&modes, "vkGetPhysicalDeviceSurfaceFormatsKHR failed: ",
                 [this](u32* count, VkPresentModeKHR* data) {
                   return vkGetPhysicalDeviceSurfacePresentModesKHR(Gpu(), m_surface, count, data);
                 }))
  {
    return false;
  }

  // FIFO with vsync (always present on conforming drivers); otherwise the lowest latency:
  // immediate (tearing), then mailbox, then whatever comes first.
  if (m_vsync && Common::Contains(modes, VK_PRESENT_MODE_FIFO_KHR))
    m_present_mode = VK_PRESENT_MODE_FIFO_KHR;
  else if (Common::Contains(modes, VK_PRESENT_MODE_IMMEDIATE_KHR))
    m_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  else if (Common::Contains(modes, VK_PRESENT_MODE_MAILBOX_KHR))
    m_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
  else
    m_present_mode = modes[0];
  return true;
}

bool SwapChain::BuildSwapChain()
{
  VkSurfaceCapabilitiesKHR caps;
  VkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Gpu(), m_surface, &caps);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed: ");
    return false;
  }
  if (!ChooseSurfaceFormat() || !ChoosePresentMode())
    return false;

  // One image more than the minimum, so one can be drawn while others wait (0 = no maximum).
  u32 image_count = caps.minImageCount + 1;
  if (caps.maxImageCount != 0)
    image_count = std::min(image_count, caps.maxImageCount);

  // An extent of 0xFFFFFFFF lets the swap chain decide the window size.
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX)
  {
    extent.width = static_cast<u32>(std::max(g_presenter->GetBackbufferWidth(), 1));
    extent.height = static_cast<u32>(std::max(g_presenter->GetBackbufferHeight(), 1));
  }
  extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);

  const VkSurfaceTransformFlagBitsKHR transform =
      (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0 ?
          VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR :
          caps.currentTransform;

  if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
  {
    ERROR_LOG_FMT(VIDEO, "Vulkan: Swap chain does not support usage as color attachment");
    return false;
  }

  // Quad-buffered stereo presents two layers.
  const u32 layers = g_ActiveConfig.stereo_mode == StereoMode::QuadBuffer ? 2 : 1;

  // The current swap chain (when resizing) is handed to the new one, then destroyed.
  const VkSwapchainKHR previous = m_swap_chain;
  m_swap_chain = VK_NULL_HANDLE;

  VkSwapchainCreateInfoKHR info = {};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = m_surface;
  info.minImageCount = image_count;
  info.imageFormat = m_surface_format.format;
  info.imageColorSpace = m_surface_format.colorSpace;
  info.imageExtent = extent;
  info.imageArrayLayers = layers;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = transform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = m_present_mode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = previous;

  // Separate graphics and present queues share the images.
  const u32 graphics_family = g_vulkan_context->GetGraphicsQueueFamilyIndex();
  const u32 present_family = g_vulkan_context->GetPresentQueueFamilyIndex();
  const std::array<u32, 2> families = {graphics_family, present_family};
  if (graphics_family != present_family)
  {
    info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    info.queueFamilyIndexCount = 2;
    info.pQueueFamilyIndices = families.data();
  }

#ifdef SUPPORTS_VULKAN_EXCLUSIVE_FULLSCREEN
  if (m_fullscreen_supported)
  {
    // Ask for application-controlled exclusive fullscreen; on failure try a plain swap chain
    // and stop offering exclusive fullscreen.
    VkSurfaceFullScreenExclusiveWin32InfoEXT monitor =
        g_vulkan_context->GetPlatformExclusiveFullscreenInfo(m_wsi);
    VkSurfaceFullScreenExclusiveInfoEXT exclusive = {};
    exclusive.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
    exclusive.pNext = &monitor;
    exclusive.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
    info.pNext = &exclusive;

    res = vkCreateSwapchainKHR(Device(), &info, nullptr, &m_swap_chain);
    if (res != VK_SUCCESS)
    {
      WARN_LOG_FMT(VIDEO, "Failed to create exclusive fullscreen swapchain, trying without.");
      info.pNext = nullptr;
      g_backend_info.bSupportsExclusiveFullscreen = false;
      m_fullscreen_supported = false;
    }
  }
#endif

  if (m_swap_chain == VK_NULL_HANDLE)
    res = vkCreateSwapchainKHR(Device(), &info, nullptr, &m_swap_chain);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateSwapchainKHR failed: ");
    return false;
  }

  // Safe right away: all work was finished before a resize.
  if (previous != VK_NULL_HANDLE)
    vkDestroySwapchainKHR(Device(), previous, nullptr);

  m_width = extent.width;
  m_height = extent.height;
  m_layers = layers;
  return true;
}

bool SwapChain::WrapImages()
{
  ASSERT(m_images.empty());

  std::vector<VkImage> images;
  u32 count = 0;
  VkResult res = vkGetSwapchainImagesKHR(Device(), m_swap_chain, &count, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetSwapchainImagesKHR failed: ");
    return false;
  }
  images.resize(count);
  res = vkGetSwapchainImagesKHR(Device(), m_swap_chain, &count, images.data());
  ASSERT(res == VK_SUCCESS);

  const TextureConfig config(m_width, m_height, 1, m_layers, 1, m_texture_format,
                             AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2DArray);
  // Looked up now so that framebuffer creation below finds them cached.
  const VkRenderPass load_pass = g_object_cache->GetRenderPass(
      m_surface_format.format, VK_FORMAT_UNDEFINED, 1, VK_ATTACHMENT_LOAD_OP_LOAD);
  const VkRenderPass clear_pass = g_object_cache->GetRenderPass(
      m_surface_format.format, VK_FORMAT_UNDEFINED, 1, VK_ATTACHMENT_LOAD_OP_CLEAR);
  if (load_pass == VK_NULL_HANDLE || clear_pass == VK_NULL_HANDLE)
  {
    PanicAlertFmt("Failed to get swap chain render passes.");
    return false;
  }

  const VkImageViewType view_type = m_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  m_images.reserve(count);
  for (VkImage vk_image : images)
  {
    Image entry;
    entry.image = vk_image;
    // The swap chain owns the image; the texture only adds a view.
    entry.texture =
        VKTexture::CreateAdopted(config, entry.image, view_type, VK_IMAGE_LAYOUT_UNDEFINED);
    if (!entry.texture)
      return false;
    entry.framebuffer = VKFramebuffer::Create(entry.texture.get(), nullptr, {});
    if (!entry.framebuffer)
    {
      entry.texture.reset();
      return false;
    }
    m_images.push_back(std::move(entry));
  }
  return true;
}

void SwapChain::ReleaseImages()
{
  for (Image& entry : m_images)
  {
    entry.framebuffer.reset();
    entry.texture.reset();
  }
  m_images.clear();
}

void SwapChain::ReleaseSwapChain()
{
  if (m_swap_chain == VK_NULL_HANDLE)
    return;
  // Exclusive fullscreen has to be given up first.
  if (m_fullscreen_now)
    SetFullscreenState(false);
  vkDestroySwapchainKHR(Device(), m_swap_chain, nullptr);
  m_swap_chain = VK_NULL_HANDLE;
}

void SwapChain::ReleaseSurface()
{
  vkDestroySurfaceKHR(g_vulkan_context->GetVulkanInstance(), m_surface, nullptr);
  m_surface = VK_NULL_HANDLE;
}

VkResult SwapChain::AcquireNextImage()
{
  const VkResult res =
      vkAcquireNextImageKHR(Device(), m_swap_chain, UINT64_MAX,
                            g_command_buffer_mgr->GetCurrentCommandBufferSemaphore(),
                            VK_NULL_HANDLE, &m_image_index);
  // Suboptimal still delivers an image (and signals the semaphore).
  m_image_valid = res >= 0;
  if (m_image_valid)
    g_command_buffer_mgr->MarkCurrentCommandBufferSemaphoreUsed();
  if (res != VK_SUCCESS && res != VK_ERROR_OUT_OF_DATE_KHR && res != VK_SUBOPTIMAL_KHR)
    LOG_VULKAN_ERROR(res, "vkAcquireNextImageKHR failed: ");
  return res;
}

bool SwapChain::ResizeSwapChain()
{
  ReleaseImages();
  if (Rebuild())
    return true;
  PanicAlertFmt("Failed to re-configure swap chain images, this is fatal (for now)");
  return false;
}

bool SwapChain::RecreateSwapChain()
{
  ReleaseImages();
  ReleaseSwapChain();
  if (Rebuild())
    return true;
  PanicAlertFmt("Failed to re-configure swap chain images, this is fatal (for now)");
  return false;
}

bool SwapChain::SetVSync(bool enabled)
{
  if (m_vsync == enabled)
    return true;
  m_vsync = enabled;
  return RecreateSwapChain();
}

bool SwapChain::SetFullscreenState(bool state)
{
#ifdef SUPPORTS_VULKAN_EXCLUSIVE_FULLSCREEN
  if (m_fullscreen_now == state)
    return true;

  if (state)
  {
    const VkResult res = vkAcquireFullScreenExclusiveModeEXT(Device(), m_swap_chain);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkAcquireFullScreenExclusiveModeEXT failed:");
      return false;
    }
    INFO_LOG_FMT(VIDEO, "Exclusive fullscreen acquired.");
  }
  else
  {
    // Releasing is reported but never fails the call.
    const VkResult res = vkReleaseFullScreenExclusiveModeEXT(Device(), m_swap_chain);
    if (res != VK_SUCCESS)
      LOG_VULKAN_ERROR(res, "vkReleaseFullScreenExclusiveModeEXT failed:");
    INFO_LOG_FMT(VIDEO, "Exclusive fullscreen released.");
  }
  m_fullscreen_now = state;
  return true;
#else
  static_cast<void>(state);
  return false;
#endif
}

bool SwapChain::RecreateSurface(void* native_handle)
{
  ReleaseImages();
  ReleaseSwapChain();
  ReleaseSurface();

  m_wsi.render_surface = native_handle;
  m_surface = CreateVulkanSurface(g_vulkan_context->GetVulkanInstance(), m_wsi);
  if (m_surface == VK_NULL_HANDLE)
    return false;

  // Validation expects a support query before a swap chain is made for the surface.
  VkBool32 can_present = VK_TRUE;
  const VkResult res = vkGetPhysicalDeviceSurfaceSupportKHR(
      Gpu(), g_vulkan_context->GetPresentQueueFamilyIndex(), m_surface, &can_present);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkGetPhysicalDeviceSurfaceSupportKHR failed: ");
    return false;
  }
  if (!can_present)
  {
    PanicAlertFmt("Recreated surface does not support presenting.");
    return false;
  }

  m_fullscreen_supported = g_vulkan_context->SupportsExclusiveFullscreen(m_wsi, m_surface);
  g_backend_info.bSupportsExclusiveFullscreen = m_fullscreen_supported;
  m_fullscreen_now = false;
  m_fullscreen_next = false;
  return Rebuild();
}
}  // namespace Vulkan
