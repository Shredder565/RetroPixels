// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#if BOOST_OS_WINDOWS
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#endif

#include <vulkan/vulkan.h>

// The renderer calls Vulkan through global function pointers named exactly like the API entry points
// (the build defines VK_NO_PROTOTYPES). They are filled in three stages:
//   InitializeGlobalVulkan    - opens the loader library and takes the few exports needed to create an instance
//   InitializeInstanceVulkan  - resolves instance level functions with vkGetInstanceProcAddr
//   InitializeDeviceVulkan    - resolves device level functions with vkGetDeviceProcAddr
bool InitializeGlobalVulkan();
bool InitializeInstanceVulkan(VkInstance instance);
bool InitializeDeviceVulkan(VkDevice device);
extern bool g_vulkan_available;

// Entry point tables. Each table takes one macro argument that is applied to every name in it.

#define PIXELU_VK_LOADER_FUNCS(X) \
	X(vkGetInstanceProcAddr) \
	X(vkCreateInstance) \
	X(vkGetDeviceProcAddr) \
	X(vkEnumerateInstanceExtensionProperties) \
	X(vkEnumerateDeviceExtensionProperties) \
	X(vkEnumerateInstanceVersion)

#define PIXELU_VK_INSTANCE_FUNCS_COMMON(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkCreateDevice) \
	X(vkDestroyDevice) \
	X(vkDeviceWaitIdle) \
	X(vkCreateDebugReportCallbackEXT) \
	X(vkGetPhysicalDeviceToolPropertiesEXT) \
	X(vkSetDebugUtilsObjectNameEXT) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkGetPhysicalDeviceSurfaceSupportKHR) \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
	X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
	X(vkGetPhysicalDeviceMemoryProperties) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceProperties2) \
	X(vkGetPhysicalDeviceFeatures2) \
	X(vkGetPhysicalDeviceFormatProperties) \
	X(vkDestroySurfaceKHR) \
	X(vkQueuePresentKHR)

// window system surface creation differs per platform
#if BOOST_OS_WINDOWS
#define PIXELU_VK_INSTANCE_FUNCS_SURFACE(X) X(vkCreateWin32SurfaceKHR)
#elif BOOST_OS_MACOS
#define PIXELU_VK_INSTANCE_FUNCS_SURFACE(X) X(vkCreateMetalSurfaceEXT)
#elif (BOOST_OS_LINUX || BOOST_OS_BSD) && defined(HAS_WAYLAND)
#define PIXELU_VK_INSTANCE_FUNCS_SURFACE(X) X(vkCreateXlibSurfaceKHR) X(vkCreateXcbSurfaceKHR) X(vkCreateWaylandSurfaceKHR)
#elif BOOST_OS_LINUX || BOOST_OS_BSD
#define PIXELU_VK_INSTANCE_FUNCS_SURFACE(X) X(vkCreateXlibSurfaceKHR) X(vkCreateXcbSurfaceKHR)
#else
#define PIXELU_VK_INSTANCE_FUNCS_SURFACE(X)
#endif

#define PIXELU_VK_INSTANCE_FUNCS(X) PIXELU_VK_INSTANCE_FUNCS_COMMON(X) PIXELU_VK_INSTANCE_FUNCS_SURFACE(X)

#define PIXELU_VK_DEVICE_FUNCS(X) \
	X(vkGetDeviceQueue) \
	/* synchronisation objects */ \
	X(vkCreateSemaphore) \
	X(vkDestroySemaphore) \
	X(vkCreateFence) \
	X(vkWaitForFences) \
	X(vkGetFenceStatus) \
	X(vkResetFences) \
	X(vkDestroyFence) \
	X(vkCreateEvent) \
	X(vkCmdSetEvent) \
	X(vkCmdWaitEvents) \
	X(vkGetEventStatus) \
	X(vkDestroyEvent) \
	/* views, shaders, framebuffers and render passes */ \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) \
	X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer) \
	X(vkCreateRenderPass) \
	X(vkDestroyRenderPass) \
	X(vkCmdBeginRenderPass) \
	X(vkCmdEndRenderPass) \
	/* command buffers and submission */ \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkFreeCommandBuffers) \
	X(vkBeginCommandBuffer) \
	X(vkResetCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkQueueSubmit) \
	/* pipelines */ \
	X(vkCreatePipelineCache) \
	X(vkMergePipelineCaches) \
	X(vkGetPipelineCacheData) \
	X(vkDestroyPipelineCache) \
	X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) \
	X(vkCreateGraphicsPipelines) \
	X(vkDestroyPipeline) \
	X(vkCmdBindPipeline) \
	/* swapchain */ \
	X(vkCreateSwapchainKHR) \
	X(vkDestroySwapchainKHR) \
	X(vkGetSwapchainImagesKHR) \
	X(vkAcquireNextImageKHR) \
	X(vkWaitForPresentKHR) \
	/* recorded commands */ \
	X(vkCmdDraw) \
	X(vkCmdDrawIndexed) \
	X(vkCmdCopyBufferToImage) \
	X(vkCmdCopyImageToBuffer) \
	X(vkCmdClearColorImage) \
	X(vkCmdClearAttachments) \
	X(vkCmdClearDepthStencilImage) \
	X(vkCmdBindIndexBuffer) \
	X(vkCmdBindVertexBuffers) \
	X(vkCmdSetViewport) \
	X(vkCmdSetScissor) \
	X(vkCmdBindDescriptorSets) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdPipelineBarrier2KHR) \
	X(vkCmdCopyBuffer) \
	X(vkCmdCopyImage) \
	X(vkCmdBlitImage) \
	X(vkCmdPushConstants) \
	X(vkCmdSetBlendConstants) \
	X(vkCmdSetDepthBias) \
	X(vkCmdBeginRenderingKHR) \
	X(vkCmdEndRenderingKHR) \
	X(vkCmdSetAttachmentFeedbackLoopEnableEXT) \
	/* queries */ \
	X(vkCreateQueryPool) \
	X(vkDestroyQueryPool) \
	X(vkCmdResetQueryPool) \
	X(vkCmdBeginQuery) \
	X(vkCmdEndQuery) \
	X(vkCmdCopyQueryPoolResults) \
	/* memory, buffers and images */ \
	X(vkAllocateMemory) \
	X(vkFreeMemory) \
	X(vkMapMemory) \
	X(vkUnmapMemory) \
	X(vkFlushMappedMemoryRanges) \
	X(vkInvalidateMappedMemoryRanges) \
	X(vkCreateBuffer) \
	X(vkDestroyBuffer) \
	X(vkBindBufferMemory) \
	X(vkGetBufferMemoryRequirements) \
	X(vkCreateImage) \
	X(vkDestroyImage) \
	X(vkGetImageMemoryRequirements) \
	X(vkBindImageMemory) \
	X(vkCreateSampler) \
	X(vkDestroySampler) \
	/* descriptors */ \
	X(vkCreateDescriptorSetLayout) \
	X(vkDestroyDescriptorSetLayout) \
	X(vkCreateDescriptorPool) \
	X(vkDestroyDescriptorPool) \
	X(vkAllocateDescriptorSets) \
	X(vkFreeDescriptorSets) \
	X(vkUpdateDescriptorSets)

#define PIXELU_VK_DECLARE_POINTER(name) extern PFN_##name name;
PIXELU_VK_LOADER_FUNCS(PIXELU_VK_DECLARE_POINTER)
PIXELU_VK_INSTANCE_FUNCS(PIXELU_VK_DECLARE_POINTER)
PIXELU_VK_DEVICE_FUNCS(PIXELU_VK_DECLARE_POINTER)
#undef PIXELU_VK_DECLARE_POINTER
