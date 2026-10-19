// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#if BOOST_OS_MACOS

#include <vulkan/vulkan.h>

// handle is the NSView* to render into; a layer-backed child view is added to it
VkSurfaceKHR CreateCocoaSurface(VkInstance instance, void* handle);

#endif
