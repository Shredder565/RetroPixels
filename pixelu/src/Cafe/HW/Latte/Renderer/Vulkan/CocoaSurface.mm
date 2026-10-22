// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/CocoaSurface.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

#include "Cafe/HW/Latte/Renderer/MetalView.h"

VkSurfaceKHR CreateCocoaSurface(VkInstance instance, void* handle)
{
	// MoltenVK presents through a CAMetalLayer, which a MetalView child covering the host view provides
	NSView* hostView = (NSView*)handle;
	MetalView* layerView = [[MetalView alloc] initWithFrame:hostView.bounds];
	layerView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	layerView.wantsLayer = YES;
	[hostView addSubview:layerView];

	VkMetalSurfaceCreateInfoEXT createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
	createInfo.pLayer = (CAMetalLayer*)layerView.layer;

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	const VkResult res = vkCreateMetalSurfaceEXT(instance, &createInfo, nullptr, &surface);
	if (res != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Cannot create a Metal Vulkan surface: {}", (sint32)res);
		throw std::runtime_error(fmt::format("Cannot create a Metal Vulkan surface: {}", (sint32)res));
	}
	return surface;
}
