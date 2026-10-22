// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The allocator's implementation lives in this translation unit.
#define VMA_IMPLEMENTATION
#include "VideoBackends/Vulkan/VulkanLoader.h"

#include <array>
#include <cstddef>
#include <string>

#include "Common/DynamicLibrary.h"

// Storage for the global entry-point pointers declared in VulkanLoader.h.
#define RP_VK_MODULE_FN(fn, needed) PFN_##fn fn;
#define RP_VK_INSTANCE_FN(fn, needed) PFN_##fn fn;
#define RP_VK_DEVICE_FN(fn, needed) PFN_##fn fn;
#include "VideoBackends/Vulkan/VulkanEntryPoints.inl"
#undef RP_VK_DEVICE_FN
#undef RP_VK_INSTANCE_FN
#undef RP_VK_MODULE_FN

namespace Vulkan
{
namespace
{
enum class Scope
{
  Module,
  Instance,
  Device,
};

struct EntryPoint
{
  Scope scope;
  const char* name;
  PFN_vkVoidFunction* slot;
  bool required;
};

#define RP_VK_ROW(scope, fn, needed)                                                               \
  EntryPoint{scope, #fn, reinterpret_cast<PFN_vkVoidFunction*>(&fn), needed},
#define RP_VK_MODULE_FN(fn, needed) RP_VK_ROW(Scope::Module, fn, needed)
#define RP_VK_INSTANCE_FN(fn, needed) RP_VK_ROW(Scope::Instance, fn, needed)
#define RP_VK_DEVICE_FN(fn, needed) RP_VK_ROW(Scope::Device, fn, needed)
const EntryPoint ENTRY_POINTS[] = {
#include "VideoBackends/Vulkan/VulkanEntryPoints.inl"
};
#undef RP_VK_DEVICE_FN
#undef RP_VK_INSTANCE_FN
#undef RP_VK_MODULE_FN
#undef RP_VK_ROW

Common::DynamicLibrary s_loader_library;

void ClearAllEntryPoints()
{
  for (const EntryPoint& entry : ENTRY_POINTS)
    *entry.slot = nullptr;
}

bool OpenLoaderLibrary()
{
  WARN_LOG_FMT(HOST_GPU, "Loading system driver");

  // Prefer the versioned name (vulkan-1), then the bare one.
  for (const std::string& file : {Common::DynamicLibrary::GetVersionedFilename("vulkan", 1),
                                  Common::DynamicLibrary::GetVersionedFilename("vulkan")})
  {
    if (s_loader_library.Open(file.c_str()))
      return true;
  }
  return false;
}

// Resolves every entry of one scope through `lookup`; reports each missing required one.
template <typename Lookup>
bool ResolveScope(Scope scope, const char* scope_name, Lookup&& lookup)
{
  bool complete = true;
  for (const EntryPoint& entry : ENTRY_POINTS)
  {
    if (entry.scope != scope)
      continue;

    *entry.slot = lookup(entry.name);
    if (*entry.slot == nullptr && entry.required)
    {
      ERROR_LOG_FMT(HOST_GPU, "Vulkan: Failed to load required {} function {}", scope_name,
                    entry.name);
      complete = false;
    }
  }
  return complete;
}

struct ResultName
{
  VkResult result;
  const char* name;
};

#define RP_VK_RESULT(r) ResultName{r, #r}
constexpr std::array RESULT_NAMES = {
    RP_VK_RESULT(VK_SUCCESS),
    RP_VK_RESULT(VK_NOT_READY),
    RP_VK_RESULT(VK_TIMEOUT),
    RP_VK_RESULT(VK_EVENT_SET),
    RP_VK_RESULT(VK_EVENT_RESET),
    RP_VK_RESULT(VK_INCOMPLETE),
    RP_VK_RESULT(VK_ERROR_OUT_OF_HOST_MEMORY),
    RP_VK_RESULT(VK_ERROR_OUT_OF_DEVICE_MEMORY),
    RP_VK_RESULT(VK_ERROR_INITIALIZATION_FAILED),
    RP_VK_RESULT(VK_ERROR_DEVICE_LOST),
    RP_VK_RESULT(VK_ERROR_MEMORY_MAP_FAILED),
    RP_VK_RESULT(VK_ERROR_LAYER_NOT_PRESENT),
    RP_VK_RESULT(VK_ERROR_EXTENSION_NOT_PRESENT),
    RP_VK_RESULT(VK_ERROR_FEATURE_NOT_PRESENT),
    RP_VK_RESULT(VK_ERROR_INCOMPATIBLE_DRIVER),
    RP_VK_RESULT(VK_ERROR_TOO_MANY_OBJECTS),
    RP_VK_RESULT(VK_ERROR_FORMAT_NOT_SUPPORTED),
    RP_VK_RESULT(VK_ERROR_SURFACE_LOST_KHR),
    RP_VK_RESULT(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR),
    RP_VK_RESULT(VK_SUBOPTIMAL_KHR),
    RP_VK_RESULT(VK_ERROR_OUT_OF_DATE_KHR),
    RP_VK_RESULT(VK_ERROR_INCOMPATIBLE_DISPLAY_KHR),
    RP_VK_RESULT(VK_ERROR_VALIDATION_FAILED_EXT),
    RP_VK_RESULT(VK_ERROR_INVALID_SHADER_NV),
};
#undef RP_VK_RESULT
}  // namespace

bool LoadVulkanLibrary(bool /*force_system_library*/)
{
  if (!s_loader_library.IsOpen() && !OpenLoaderLibrary())
    return false;

  // The global functions are exported by the library itself. Give up at the first missing
  // required one and leave nothing half-loaded behind.
  for (const EntryPoint& entry : ENTRY_POINTS)
  {
    if (entry.scope != Scope::Module)
      continue;
    if (!s_loader_library.GetSymbol(entry.name, entry.slot) && entry.required)
    {
      ERROR_LOG_FMT(VIDEO, "Vulkan: Failed to load required module function {}", entry.name);
      ClearAllEntryPoints();
      s_loader_library.Close();
      return false;
    }
  }
  return true;
}

void UnloadVulkanLibrary()
{
  s_loader_library.Close();
  if (!s_loader_library.IsOpen())
    ClearAllEntryPoints();
}

bool LoadVulkanInstanceFunctions(VkInstance instance)
{
  return ResolveScope(Scope::Instance, "instance", [instance](const char* name) {
    return vkGetInstanceProcAddr(instance, name);
  });
}

bool LoadVulkanDeviceFunctions(VkDevice device)
{
  return ResolveScope(Scope::Device, "device",
                      [device](const char* name) { return vkGetDeviceProcAddr(device, name); });
}

const char* VkResultToString(VkResult res)
{
  for (const ResultName& entry : RESULT_NAMES)
  {
    if (entry.result == res)
      return entry.name;
  }
  return "UNKNOWN_VK_RESULT";
}

void LogVulkanResult(Common::Log::LogLevel level, const char* func_name, const int line,
                     VkResult res, const char* msg)
{
  GENERIC_LOG_FMT(Common::Log::LogType::VIDEO, level, "({}:{}) {} ({}: {})", func_name, line, msg,
                  static_cast<int>(res), VkResultToString(res));
}

}  // namespace Vulkan
