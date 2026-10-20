// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Vulkan renderer for the PixelU imgui overlay (notifications, FPS counter and the
// in-game menus). VulkanRenderer owns the device, the descriptor pool and the render
// pass; this backend only builds its own pipeline and textures on top of them.
#pragma once

#include <imgui.h>
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "util/math/vector2.h"

// Everything the backend borrows from the renderer. Value-initialise before filling in.
struct ImGui_ImplVulkan_InitInfo
{
	VkInstance Instance;
	VkPhysicalDevice PhysicalDevice;
	VkDevice Device;
	uint32_t QueueFamily;
	VkQueue Queue;
	VkPipelineCache PipelineCache;
	VkDescriptorPool DescriptorPool; // texture descriptor sets come from (and go back to) here
	uint32_t MinImageCount; // at least 2
	uint32_t ImageCount; // one set of geometry buffers per swapchain image
	const VkAllocationCallbacks* Allocator;
	void (*CheckVkResultFn)(VkResult err); // optional; told about every Vulkan result
};

// Lifetime: Init once the overlay render pass exists, Shutdown before that render pass
// (or the descriptor pool) is replaced.
IMGUI_IMPL_API bool ImGui_ImplVulkan_Init(ImGui_ImplVulkan_InitInfo* info, VkRenderPass render_pass);
IMGUI_IMPL_API void ImGui_ImplVulkan_Shutdown();

// Per frame: NewFrame begins the overlay render pass on the given framebuffer, then
// RenderDrawData records the overlay into the same command buffer. The caller ends the pass.
IMGUI_IMPL_API void ImGui_ImplVulkan_NewFrame(VkCommandBuffer command_buffer, VkFramebuffer framebuffer, VkExtent2D extent);
IMGUI_IMPL_API void ImGui_ImplVulkan_RenderDrawData(ImDrawData* draw_data, VkCommandBuffer command_buffer);

// Font atlas. CreateFontsTexture records the upload into command_buffer and does nothing
// while the atlas already exists.
IMGUI_IMPL_API bool ImGui_ImplVulkan_CreateFontsTexture(VkCommandBuffer command_buffer);
IMGUI_IMPL_API void ImGui_ImplVulkan_DestroyFontsTexture();
IMGUI_IMPL_API void ImGui_ImplVulkan_DestroyFontUploadObjects();
IMGUI_IMPL_API void ImGui_ImplVulkan_SetMinImageCount(uint32_t min_image_count);

// Textures. AddTexture only makes a descriptor set for an existing view (its result is that
// VkDescriptorSet); GenerateTexture uploads tightly packed RGBA8 pixels into a new texture
// that DeleteTexture releases again.
IMGUI_IMPL_API ImTextureID ImGui_ImplVulkan_AddTexture(VkSampler sampler, VkImageView image_view, VkImageLayout image_layout);
IMGUI_IMPL_API ImTextureID ImGui_ImplVulkan_GenerateTexture(VkCommandBuffer commandBuffer, const std::vector<uint8>& data, const Vector2i& size);
IMGUI_IMPL_API void ImGui_ImplVulkan_DeleteTexture(ImTextureID id);
