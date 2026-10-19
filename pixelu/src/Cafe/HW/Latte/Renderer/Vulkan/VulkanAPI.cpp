// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

#if BOOST_OS_LINUX || BOOST_OS_MACOS || BOOST_OS_BSD
#include <dlfcn.h>
#endif

// storage for every entry point pointer declared in the header
#define PIXELU_VK_DEFINE_POINTER(name) NOEXPORT PFN_##name name = nullptr;
PIXELU_VK_LOADER_FUNCS(PIXELU_VK_DEFINE_POINTER)
PIXELU_VK_INSTANCE_FUNCS(PIXELU_VK_DEFINE_POINTER)
PIXELU_VK_DEVICE_FUNCS(PIXELU_VK_DEFINE_POINTER)
#undef PIXELU_VK_DEFINE_POINTER

bool g_vulkan_available = false;

// Called once per presented TV frame. Per-function CPU timing is not built into PixelU, so there is nothing to print.
void VulkanBenchmarkPrintResults()
{
}

namespace
{
#if BOOST_OS_WINDOWS
	using LoaderHandle = HMODULE;

	// LoadLibrary takes a reference each call, GetModuleHandle only looks up an already loaded module
	LoaderHandle OpenLoader(bool alreadyLoaded)
	{
		return alreadyLoaded ? GetModuleHandleA("vulkan-1.dll") : LoadLibraryA("vulkan-1.dll");
	}

	void* LoaderSymbol(LoaderHandle lib, const char* name)
	{
		return reinterpret_cast<void*>(GetProcAddress(lib, name));
	}
#else
	using LoaderHandle = void*;

	LoaderHandle OpenLoader(bool /*alreadyLoaded*/)
	{
#if BOOST_OS_MACOS
		return dlopen("libMoltenVK.dylib", RTLD_NOW);
#else
		// prefer the unversioned development name, fall back to the runtime soname
		LoaderHandle lib = dlopen("libvulkan.so", RTLD_NOW);
		if (!lib)
			lib = dlopen("libvulkan.so.1", RTLD_NOW);
		return lib;
#endif
	}

	void* LoaderSymbol(LoaderHandle lib, const char* name)
	{
		return dlsym(lib, name);
	}
#endif
}

bool InitializeGlobalVulkan()
{
	LoaderHandle lib = OpenLoader(false);
	if (g_vulkan_available)
		return true;
	if (!lib)
	{
#if BOOST_OS_WINDOWS
		cemuLog_log(LogType::Force, "Vulkan loader not available. Outdated graphics driver or Vulkan runtime not installed?");
#else
		cemuLog_log(LogType::Force, "Vulkan loader not available.");
#endif
		return false;
	}

#define PIXELU_VK_RESOLVE_EXPORT(name) name = reinterpret_cast<PFN_##name>(LoaderSymbol(lib, #name));
	PIXELU_VK_LOADER_FUNCS(PIXELU_VK_RESOLVE_EXPORT)
#undef PIXELU_VK_RESOLVE_EXPORT

	// a loader without vkEnumerateInstanceVersion predates Vulkan 1.1, which PixelU requires
	if (!vkEnumerateInstanceVersion)
	{
		cemuLog_log(LogType::Force, "vkEnumerateInstanceVersion not available. Outdated graphics driver or Vulkan runtime?");
#if BOOST_OS_WINDOWS
		FreeLibrary(lib);
#endif
		return false;
	}

	g_vulkan_available = true;
	return true;
}

bool InitializeInstanceVulkan(VkInstance instance)
{
	if (!OpenLoader(true))
		return false;
#define PIXELU_VK_RESOLVE_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
	PIXELU_VK_INSTANCE_FUNCS(PIXELU_VK_RESOLVE_INSTANCE)
#undef PIXELU_VK_RESOLVE_INSTANCE
	return true;
}

bool InitializeDeviceVulkan(VkDevice device)
{
	if (!OpenLoader(true))
		return false;
#define PIXELU_VK_RESOLVE_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
	PIXELU_VK_DEVICE_FUNCS(PIXELU_VK_RESOLVE_DEVICE)
#undef PIXELU_VK_RESOLVE_DEVICE
	return true;
}
