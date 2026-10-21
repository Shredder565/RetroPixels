// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// The backend never links against vulkan-1 directly: every entry point is a global function
// pointer filled at run time (see VulkanEntryPoints.inl), so the prototypes must stay hidden.
#define VK_NO_PROTOTYPES

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "vulkan/vulkan.h"

// VK_EXT_full_screen_exclusive needs a Win32 monitor handle, so it is only offered on Windows.
#if defined(WIN32)
#define SUPPORTS_VULKAN_EXCLUSIVE_FULLSCREEN 1
#endif

// One global pointer per entry point, named exactly like the Vulkan function it stands for.
#define RP_VK_MODULE_FN(fn, needed) extern PFN_##fn fn;
#define RP_VK_INSTANCE_FN(fn, needed) extern PFN_##fn fn;
#define RP_VK_DEVICE_FN(fn, needed) extern PFN_##fn fn;
#include "VideoBackends/Vulkan/VulkanEntryPoints.inl"
#undef RP_VK_DEVICE_FN
#undef RP_VK_INSTANCE_FN
#undef RP_VK_MODULE_FN

// The Vulkan Memory Allocator is pulled in here, after the entry-point pointers exist, so that
// its static function table binds to them. Its own code is noisy under our warning levels.
#ifdef _MSVC_LANG
#pragma warning(push, 4)
#pragma warning(disable : 4189)
#pragma warning(disable : 4505)
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#endif

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define VMA_VULKAN_VERSION 1002000
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#undef VK_NO_PROTOTYPES
#include "vk_mem_alloc.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifdef _MSVC_LANG
#pragma warning(pop)
#endif

#include "Common/Logging/Log.h"

namespace Vulkan
{
// Opens the system Vulkan loader and fetches the global-level entry points.
bool LoadVulkanLibrary(bool force_system_library = false);
// Fills the instance-level pointers through vkGetInstanceProcAddr.
bool LoadVulkanInstanceFunctions(VkInstance instance);
// Fills the device-level pointers through vkGetDeviceProcAddr.
bool LoadVulkanDeviceFunctions(VkDevice device);
// Closes the loader again and clears every pointer.
void UnloadVulkanLibrary();

// The VK_* enumerator name of a result code ("UNKNOWN_VK_RESULT" for anything else).
const char* VkResultToString(VkResult res);
// Writes "(func:line) msg (code: name)" to the video log at the given level.
void LogVulkanResult(Common::Log::LogLevel level, const char* func_name, const int line,
                     VkResult res, const char* msg);

#define LOG_VULKAN_ERROR(res, msg)                                                                 \
  LogVulkanResult(Common::Log::LogLevel::LERROR, __func__, __LINE__, res, msg)

}  // namespace Vulkan
