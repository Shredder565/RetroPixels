// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/WindowSystemInfo.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoCommon/TextureConfig.h"

namespace Vulkan
{
class CommandBufferManager;
class ObjectCache;
class VKTexture;
class VKFramebuffer;

// The window surface, its swap chain, and a texture plus framebuffer for every swap chain
// image. Also tracks exclusive fullscreen where the platform has it.
class SwapChain
{
public:
  SwapChain(const WindowSystemInfo& wsi, VkSurfaceKHR surface, bool vsync);
  ~SwapChain();

  // A surface for the window in `wsi`, or VK_NULL_HANDLE.
  static VkSurfaceKHR CreateVulkanSurface(VkInstance instance, const WindowSystemInfo& wsi);

  // Builds the swap chain and image objects on an existing surface (taken over).
  static std::unique_ptr<SwapChain> Create(const WindowSystemInfo& wsi, VkSurfaceKHR surface,
                                           bool vsync);

  VkSurfaceKHR GetSurface() const { return m_surface; }
  VkSurfaceFormatKHR GetSurfaceFormat() const { return m_surface_format; }
  AbstractTextureFormat GetTextureFormat() const { return m_texture_format; }
  VkSwapchainKHR GetSwapChain() const { return m_swap_chain; }
  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }
  u32 GetCurrentImageIndex() const { return m_image_index; }
  bool IsCurrentImageValid() const { return m_image_valid; }
  size_t GetSwapChainImageCount() const { return m_images.size(); }
  VkImage GetCurrentImage() const { return m_images[m_image_index].image; }
  VKTexture* GetCurrentTexture() const { return m_images[m_image_index].texture.get(); }
  VKFramebuffer* GetCurrentFramebuffer() const { return m_images[m_image_index].framebuffer.get(); }

  // Acquires the next image, signalling the current command buffer's semaphore.
  VkResult AcquireNextImage();

  bool RecreateSurface(void* native_handle);
  // Rebuilds the swap chain from the old one (window size change).
  bool ResizeSwapChain();
  // Destroys the swap chain first, then builds a new one.
  bool RecreateSwapChain();

  // Recreates the swap chain when the setting changes; may fail.
  bool SetVSync(bool enabled);

  bool IsFullscreenSupported() const { return m_fullscreen_supported; }

  // The "next" state can be read and written from any thread.
  bool GetCurrentFullscreenState() const { return m_fullscreen_now; }
  bool GetNextFullscreenState() const { return m_fullscreen_next; }
  void SetNextFullscreenState(bool state) { m_fullscreen_next = state; }

  // Acquires or releases exclusive fullscreen; video thread only.
  bool SetFullscreenState(bool state);

private:
  struct Image
  {
    VkImage image{};
    std::unique_ptr<VKTexture> texture;
    std::unique_ptr<VKFramebuffer> framebuffer;
  };

  bool ChooseSurfaceFormat();
  bool ChoosePresentMode();
  bool BuildSwapChain();
  void ReleaseSwapChain();
  bool WrapImages();
  void ReleaseImages();
  void ReleaseSurface();
  bool Rebuild();

  WindowSystemInfo m_wsi;
  VkSurfaceKHR m_surface = VK_NULL_HANDLE;
  VkSurfaceFormatKHR m_surface_format = {};
  VkPresentModeKHR m_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  AbstractTextureFormat m_texture_format = AbstractTextureFormat::Undefined;
  bool m_vsync = false;
  bool m_fullscreen_supported = false;
  bool m_fullscreen_now = false;
  bool m_fullscreen_next = false;
  bool m_image_valid = false;

  VkSwapchainKHR m_swap_chain = VK_NULL_HANDLE;
  std::vector<Image> m_images;
  u32 m_image_index = 0;

  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_layers = 0;
};

}  // namespace Vulkan
