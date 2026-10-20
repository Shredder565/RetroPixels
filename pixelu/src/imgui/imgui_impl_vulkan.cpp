// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Vulkan renderer for the PixelU imgui overlay. See imgui_impl_vulkan.h for the call order.
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"

#include <cstring>
#include <vector>

namespace
{
	// The overlay shaders, compiled with glslangValidator -V from this GLSL:
	//
	//   overlay.vert                                    overlay.frag
	//   #version 450                                    #version 450
	//   layout(location = 0) in vec2 inPosition;        layout(set = 0, binding = 0) uniform sampler2D atlas;
	//   layout(location = 1) in vec2 inUV;              layout(location = 0) in vec4 fragColor;
	//   layout(location = 2) in vec4 inColor;           layout(location = 1) in vec2 fragUV;
	//   layout(push_constant) uniform Transform         layout(location = 0) out vec4 outColor;
	//     { vec2 scale; vec2 offset; } xf;              void main()
	//   layout(location = 0) out vec4 fragColor;        {
	//   layout(location = 1) out vec2 fragUV;             outColor = fragColor * texture(atlas, fragUV);
	//   void main()                                     }
	//   {
	//     fragColor = inColor;
	//     fragUV = inUV;
	//     gl_Position = vec4(inPosition * xf.scale + xf.offset, 0.0, 1.0);
	//   }
	const uint32_t kOverlayVertexSpirv[] = {
		0x07230203,0x00010000,0x0008000b,0x0000002e,0x00000000,0x00020011,0x00000001,0x0006000b,
		0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
		0x000b000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x00000009,0x0000000b,0x0000000f,
		0x00000011,0x00000018,0x0000001b,0x00040047,0x00000009,0x0000001e,0x00000000,0x00040047,
		0x0000000b,0x0000001e,0x00000002,0x00040047,0x0000000f,0x0000001e,0x00000001,0x00040047,
		0x00000011,0x0000001e,0x00000001,0x00030047,0x00000016,0x00000002,0x00050048,0x00000016,
		0x00000000,0x0000000b,0x00000000,0x00050048,0x00000016,0x00000001,0x0000000b,0x00000001,
		0x00050048,0x00000016,0x00000002,0x0000000b,0x00000003,0x00050048,0x00000016,0x00000003,
		0x0000000b,0x00000004,0x00040047,0x0000001b,0x0000001e,0x00000000,0x00030047,0x0000001d,
		0x00000002,0x00050048,0x0000001d,0x00000000,0x00000023,0x00000000,0x00050048,0x0000001d,
		0x00000001,0x00000023,0x00000008,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,
		0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,
		0x00000008,0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x00040020,
		0x0000000a,0x00000001,0x00000007,0x0004003b,0x0000000a,0x0000000b,0x00000001,0x00040017,
		0x0000000d,0x00000006,0x00000002,0x00040020,0x0000000e,0x00000003,0x0000000d,0x0004003b,
		0x0000000e,0x0000000f,0x00000003,0x00040020,0x00000010,0x00000001,0x0000000d,0x0004003b,
		0x00000010,0x00000011,0x00000001,0x00040015,0x00000013,0x00000020,0x00000000,0x0004002b,
		0x00000013,0x00000014,0x00000001,0x0004001c,0x00000015,0x00000006,0x00000014,0x0006001e,
		0x00000016,0x00000007,0x00000006,0x00000015,0x00000015,0x00040020,0x00000017,0x00000003,
		0x00000016,0x0004003b,0x00000017,0x00000018,0x00000003,0x00040015,0x00000019,0x00000020,
		0x00000001,0x0004002b,0x00000019,0x0000001a,0x00000000,0x0004003b,0x00000010,0x0000001b,
		0x00000001,0x0004001e,0x0000001d,0x0000000d,0x0000000d,0x00040020,0x0000001e,0x00000009,
		0x0000001d,0x0004003b,0x0000001e,0x0000001f,0x00000009,0x00040020,0x00000020,0x00000009,
		0x0000000d,0x0004002b,0x00000019,0x00000024,0x00000001,0x0004002b,0x00000006,0x00000028,
		0x00000000,0x0004002b,0x00000006,0x00000029,0x3f800000,0x00050036,0x00000002,0x00000004,
		0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003d,0x00000007,0x0000000c,0x0000000b,
		0x0003003e,0x00000009,0x0000000c,0x0004003d,0x0000000d,0x00000012,0x00000011,0x0003003e,
		0x0000000f,0x00000012,0x0004003d,0x0000000d,0x0000001c,0x0000001b,0x00050041,0x00000020,
		0x00000021,0x0000001f,0x0000001a,0x0004003d,0x0000000d,0x00000022,0x00000021,0x00050085,
		0x0000000d,0x00000023,0x0000001c,0x00000022,0x00050041,0x00000020,0x00000025,0x0000001f,
		0x00000024,0x0004003d,0x0000000d,0x00000026,0x00000025,0x00050081,0x0000000d,0x00000027,
		0x00000023,0x00000026,0x00050051,0x00000006,0x0000002a,0x00000027,0x00000000,0x00050051,
		0x00000006,0x0000002b,0x00000027,0x00000001,0x00070050,0x00000007,0x0000002c,0x0000002a,
		0x0000002b,0x00000028,0x00000029,0x00050041,0x00000008,0x0000002d,0x00000018,0x0000001a,
		0x0003003e,0x0000002d,0x0000002c,0x000100fd,0x00010038
	};
	const uint32_t kOverlayFragmentSpirv[] = {
		0x07230203,0x00010000,0x0008000b,0x00000018,0x00000000,0x00020011,0x00000001,0x0006000b,
		0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
		0x0008000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x0000000b,0x00000014,
		0x00030010,0x00000004,0x00000007,0x00040047,0x00000009,0x0000001e,0x00000000,0x00040047,
		0x0000000b,0x0000001e,0x00000000,0x00040047,0x00000010,0x00000021,0x00000000,0x00040047,
		0x00000010,0x00000022,0x00000000,0x00040047,0x00000014,0x0000001e,0x00000001,0x00020013,
		0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,
		0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000003,0x00000007,0x0004003b,
		0x00000008,0x00000009,0x00000003,0x00040020,0x0000000a,0x00000001,0x00000007,0x0004003b,
		0x0000000a,0x0000000b,0x00000001,0x00090019,0x0000000d,0x00000006,0x00000001,0x00000000,
		0x00000000,0x00000000,0x00000001,0x00000000,0x0003001b,0x0000000e,0x0000000d,0x00040020,
		0x0000000f,0x00000000,0x0000000e,0x0004003b,0x0000000f,0x00000010,0x00000000,0x00040017,
		0x00000012,0x00000006,0x00000002,0x00040020,0x00000013,0x00000001,0x00000012,0x0004003b,
		0x00000013,0x00000014,0x00000001,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,
		0x000200f8,0x00000005,0x0004003d,0x00000007,0x0000000c,0x0000000b,0x0004003d,0x0000000e,
		0x00000011,0x00000010,0x0004003d,0x00000012,0x00000015,0x00000014,0x00050057,0x00000007,
		0x00000016,0x00000011,0x00000015,0x00050085,0x00000007,0x00000017,0x0000000c,0x00000016,
		0x0003003e,0x00000009,0x00000017,0x000100fd,0x00010038
	};

	// What an ImTextureID handed out by this backend points at. The font atlas only fills in
	// the descriptor set; its image lives in Backend below.
	struct OverlayTexture
	{
		VkDescriptorSet set;
		VkImage image;
		VkDeviceMemory imageMemory;
		VkImageView view;
		VkSampler sampler;
		VkBuffer staging;
		VkDeviceMemory stagingMemory;
	};

	struct HostBuffer
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize capacity = 0;
	};

	// Geometry for one swapchain image, so a frame still in flight keeps its buffers.
	struct FrameGeometry
	{
		HostBuffer vertices;
		HostBuffer indices;
	};

	struct Backend
	{
		ImGui_ImplVulkan_InitInfo info{};
		VkRenderPass renderPass = VK_NULL_HANDLE;
		VkDeviceSize sizeGranularity = 256; // geometry buffer sizes are rounded up to this
		VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkSampler fontSampler = VK_NULL_HANDLE;
		VkImage fontImage = VK_NULL_HANDLE;
		VkDeviceMemory fontMemory = VK_NULL_HANDLE;
		VkImageView fontView = VK_NULL_HANDLE;
		VkBuffer fontStaging = VK_NULL_HANDLE;
		VkDeviceMemory fontStagingMemory = VK_NULL_HANDLE;
		std::vector<FrameGeometry> frames;
		size_t frameCursor = 0;
	};
	Backend s_vk;

	constexpr VkIndexType kIndexType = sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
	constexpr VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;

	void Report(VkResult result)
	{
		if (s_vk.info.CheckVkResultFn)
			s_vk.info.CheckVkResultFn(result);
	}

	// First memory type allowed by allowedTypes that has all the wanted property bits.
	uint32 FindMemoryType(VkMemoryPropertyFlags wanted, uint32 allowedTypes)
	{
		VkPhysicalDeviceMemoryProperties props;
		vkGetPhysicalDeviceMemoryProperties(s_vk.info.PhysicalDevice, &props);
		for (uint32 type = 0; type < props.memoryTypeCount; ++type)
		{
			const bool allowed = (allowedTypes >> type) & 1;
			if (allowed && (props.memoryTypes[type].propertyFlags & wanted) == wanted)
				return type;
		}
		return 0xFFFFFFFF;
	}

	VkDeviceMemory AllocateFor(const VkMemoryRequirements& req, VkMemoryPropertyFlags wanted)
	{
		VkMemoryAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = FindMemoryType(wanted, req.memoryTypeBits);
		VkDeviceMemory memory = VK_NULL_HANDLE;
		Report(vkAllocateMemory(s_vk.info.Device, &alloc, s_vk.info.Allocator, &memory));
		return memory;
	}

	// Host visible buffer with its own memory; returns the buffer's alignment requirement.
	VkDeviceSize CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory)
	{
		VkBufferCreateInfo create{};
		create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		create.size = size;
		create.usage = usage;
		create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		Report(vkCreateBuffer(s_vk.info.Device, &create, s_vk.info.Allocator, &buffer));
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(s_vk.info.Device, buffer, &req);
		memory = AllocateFor(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		Report(vkBindBufferMemory(s_vk.info.Device, buffer, memory, 0));
		return req.alignment;
	}

	void CopyToHostMemory(VkDeviceMemory memory, const void* src, size_t bytes)
	{
		void* dst = nullptr;
		Report(vkMapMemory(s_vk.info.Device, memory, 0, bytes, 0, &dst));
		memcpy(dst, src, bytes);
		VkMappedMemoryRange range{};
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = memory;
		range.size = bytes;
		Report(vkFlushMappedMemoryRanges(s_vk.info.Device, 1, &range));
		vkUnmapMemory(s_vk.info.Device, memory);
	}

	void ReleaseHostBuffer(HostBuffer& hb)
	{
		if (hb.buffer != VK_NULL_HANDLE)
			vkDestroyBuffer(s_vk.info.Device, hb.buffer, s_vk.info.Allocator);
		if (hb.memory != VK_NULL_HANDLE)
			vkFreeMemory(s_vk.info.Device, hb.memory, s_vk.info.Allocator);
		hb = HostBuffer{};
	}

	// Makes room for at least `bytes`. Growing waits for the GPU, since an older frame may
	// still be reading the buffer that gets replaced.
	void EnsureCapacity(HostBuffer& hb, size_t bytes, VkBufferUsageFlags usage)
	{
		if (hb.buffer != VK_NULL_HANDLE && hb.capacity >= bytes)
			return;
		vkDeviceWaitIdle(s_vk.info.Device);
		ReleaseHostBuffer(hb);
		const VkDeviceSize step = s_vk.sizeGranularity;
		const VkDeviceSize rounded = (((VkDeviceSize)bytes + step - 1) / step) * step;
		const VkDeviceSize alignment = CreateHostBuffer(rounded, usage, hb.buffer, hb.memory);
		if (alignment > s_vk.sizeGranularity)
			s_vk.sizeGranularity = alignment;
		hb.capacity = bytes;
	}

	void ReleaseAllGeometry()
	{
		for (FrameGeometry& fg : s_vk.frames)
		{
			ReleaseHostBuffer(fg.vertices);
			ReleaseHostBuffer(fg.indices);
		}
		s_vk.frames.clear();
		s_vk.frameCursor = 0;
	}

	// Device local RGBA8 image (sampled, transfer destination) plus a view of it.
	void CreateSampledImage(uint32 width, uint32 height, VkImage& image, VkDeviceMemory& memory, VkImageView& view)
	{
		VkImageCreateInfo create{};
		create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		create.imageType = VK_IMAGE_TYPE_2D;
		create.format = kTextureFormat;
		create.extent = {width, height, 1};
		create.mipLevels = 1;
		create.arrayLayers = 1;
		create.samples = VK_SAMPLE_COUNT_1_BIT;
		create.tiling = VK_IMAGE_TILING_OPTIMAL;
		create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		Report(vkCreateImage(s_vk.info.Device, &create, s_vk.info.Allocator, &image));
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(s_vk.info.Device, image, &req);
		memory = AllocateFor(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		Report(vkBindImageMemory(s_vk.info.Device, image, memory, 0));

		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = kTextureFormat;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		Report(vkCreateImageView(s_vk.info.Device, &viewInfo, s_vk.info.Allocator, &view));
	}

	// Bilinear, wrapping, no LOD limits worth mentioning.
	VkSampler CreateOverlaySampler()
	{
		VkSamplerCreateInfo create{};
		create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		create.magFilter = VK_FILTER_LINEAR;
		create.minFilter = VK_FILTER_LINEAR;
		create.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		create.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		create.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		create.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		create.minLod = -1000.0f;
		create.maxLod = 1000.0f;
		create.maxAnisotropy = 1.0f;
		VkSampler sampler = VK_NULL_HANDLE;
		Report(vkCreateSampler(s_vk.info.Device, &create, s_vk.info.Allocator, &sampler));
		return sampler;
	}

	void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
		VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
	{
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = srcAccess;
		barrier.dstAccessMask = dstAccess;
		barrier.oldLayout = from;
		barrier.newLayout = to;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	// Records staging buffer -> image, leaving the image ready for the fragment shader.
	void RecordImageUpload(VkCommandBuffer cmd, VkBuffer staging, VkImage image, uint32 width, uint32 height)
	{
		ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy copy{};
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent = {width, height, 1};
		vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		ImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	VkShaderModule LoadShader(const uint32_t* words, size_t bytes)
	{
		VkShaderModuleCreateInfo create{};
		create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		create.codeSize = bytes;
		create.pCode = words;
		VkShaderModule module = VK_NULL_HANDLE;
		Report(vkCreateShaderModule(s_vk.info.Device, &create, s_vk.info.Allocator, &module));
		return module;
	}

	void CreatePipeline()
	{
		const VkShaderModule vertexShader = LoadShader(kOverlayVertexSpirv, sizeof(kOverlayVertexSpirv));
		const VkShaderModule fragmentShader = LoadShader(kOverlayFragmentSpirv, sizeof(kOverlayFragmentSpirv));

		VkPipelineShaderStageCreateInfo stages[2]{};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertexShader;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragmentShader;
		stages[1].pName = "main";

		// ImDrawVert: pos (2 floats), uv (2 floats), col (packed RGBA8)
		VkVertexInputBindingDescription vertexBinding{};
		vertexBinding.binding = 0;
		vertexBinding.stride = sizeof(ImDrawVert);
		vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		const VkVertexInputAttributeDescription attributes[3] = {
			{0, 0, VK_FORMAT_R32G32_SFLOAT, (uint32)offsetof(ImDrawVert, pos)},
			{1, 0, VK_FORMAT_R32G32_SFLOAT, (uint32)offsetof(ImDrawVert, uv)},
			{2, 0, VK_FORMAT_R8G8B8A8_UNORM, (uint32)offsetof(ImDrawVert, col)},
		};
		VkPipelineVertexInputStateCreateInfo vertexInput{};
		vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInput.vertexBindingDescriptionCount = 1;
		vertexInput.pVertexBindingDescriptions = &vertexBinding;
		vertexInput.vertexAttributeDescriptionCount = 3;
		vertexInput.pVertexAttributeDescriptions = attributes;

		VkPipelineInputAssemblyStateCreateInfo assembly{};
		assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		// viewport and scissor are set while recording
		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;
		const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = 2;
		dynamicState.pDynamicStates = dynamics;

		VkPipelineRasterizationStateCreateInfo raster{};
		raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample{};
		multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineDepthStencilStateCreateInfo depthStencil{};
		depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

		// straight alpha over the game image; destination alpha becomes (1 - a) * a_src
		VkPipelineColorBlendAttachmentState blend{};
		blend.blendEnable = VK_TRUE;
		blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.colorBlendOp = VK_BLEND_OP_ADD;
		blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend.alphaBlendOp = VK_BLEND_OP_ADD;
		blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo blendState{};
		blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		blendState.attachmentCount = 1;
		blendState.pAttachments = &blend;

		VkGraphicsPipelineCreateInfo create{};
		create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		create.stageCount = 2;
		create.pStages = stages;
		create.pVertexInputState = &vertexInput;
		create.pInputAssemblyState = &assembly;
		create.pViewportState = &viewportState;
		create.pRasterizationState = &raster;
		create.pMultisampleState = &multisample;
		create.pDepthStencilState = &depthStencil;
		create.pColorBlendState = &blendState;
		create.pDynamicState = &dynamicState;
		create.layout = s_vk.pipelineLayout;
		create.renderPass = s_vk.renderPass;
		Report(vkCreateGraphicsPipelines(s_vk.info.Device, s_vk.info.PipelineCache, 1, &create, s_vk.info.Allocator, &s_vk.pipeline));

		vkDestroyShaderModule(s_vk.info.Device, vertexShader, s_vk.info.Allocator);
		vkDestroyShaderModule(s_vk.info.Device, fragmentShader, s_vk.info.Allocator);
	}

	// The sampler and layouts survive a Shutdown/Init pair only if they were never destroyed;
	// the pipeline is always rebuilt because it is tied to the render pass.
	void CreateDeviceObjects()
	{
		if (s_vk.fontSampler == VK_NULL_HANDLE)
			s_vk.fontSampler = CreateOverlaySampler();
		if (s_vk.setLayout == VK_NULL_HANDLE)
		{
			VkDescriptorSetLayoutBinding binding{};
			binding.binding = 0;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			VkDescriptorSetLayoutCreateInfo create{};
			create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			create.bindingCount = 1;
			create.pBindings = &binding;
			Report(vkCreateDescriptorSetLayout(s_vk.info.Device, &create, s_vk.info.Allocator, &s_vk.setLayout));
		}
		if (s_vk.pipelineLayout == VK_NULL_HANDLE)
		{
			VkPushConstantRange transform{};
			transform.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
			transform.offset = 0;
			transform.size = sizeof(float) * 4; // scale.xy, offset.xy
			VkPipelineLayoutCreateInfo create{};
			create.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			create.setLayoutCount = 1;
			create.pSetLayouts = &s_vk.setLayout;
			create.pushConstantRangeCount = 1;
			create.pPushConstantRanges = &transform;
			Report(vkCreatePipelineLayout(s_vk.info.Device, &create, s_vk.info.Allocator, &s_vk.pipelineLayout));
		}
		CreatePipeline();
	}

	void DestroyFontImage()
	{
		if (s_vk.fontView != VK_NULL_HANDLE)
			vkDestroyImageView(s_vk.info.Device, s_vk.fontView, s_vk.info.Allocator);
		if (s_vk.fontImage != VK_NULL_HANDLE)
			vkDestroyImage(s_vk.info.Device, s_vk.fontImage, s_vk.info.Allocator);
		if (s_vk.fontMemory != VK_NULL_HANDLE)
			vkFreeMemory(s_vk.info.Device, s_vk.fontMemory, s_vk.info.Allocator);
		s_vk.fontView = VK_NULL_HANDLE;
		s_vk.fontImage = VK_NULL_HANDLE;
		s_vk.fontMemory = VK_NULL_HANDLE;
	}

	// Descriptor sets are not freed here: by the time the renderer shuts the overlay down it
	// has already destroyed (or is about to destroy) the pool they came from.
	void DestroyDeviceObjects()
	{
		ReleaseAllGeometry();
		ImGui_ImplVulkan_DestroyFontUploadObjects();
		DestroyFontImage();
		if (s_vk.fontSampler != VK_NULL_HANDLE)
			vkDestroySampler(s_vk.info.Device, s_vk.fontSampler, s_vk.info.Allocator);
		if (s_vk.setLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(s_vk.info.Device, s_vk.setLayout, s_vk.info.Allocator);
		if (s_vk.pipelineLayout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(s_vk.info.Device, s_vk.pipelineLayout, s_vk.info.Allocator);
		if (s_vk.pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(s_vk.info.Device, s_vk.pipeline, s_vk.info.Allocator);
		s_vk.fontSampler = VK_NULL_HANDLE;
		s_vk.setLayout = VK_NULL_HANDLE;
		s_vk.pipelineLayout = VK_NULL_HANDLE;
		s_vk.pipeline = VK_NULL_HANDLE;
	}

	// Pipeline, geometry buffers, full-framebuffer viewport and the display -> clip transform.
	// Also used again whenever a draw list asks for ImDrawCallback_ResetRenderState.
	void BindOverlayState(const ImDrawData* drawData, VkCommandBuffer cmd, const FrameGeometry& geometry, int fbWidth, int fbHeight)
	{
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipeline);
		const VkDeviceSize start = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &geometry.vertices.buffer, &start);
		vkCmdBindIndexBuffer(cmd, geometry.indices.buffer, 0, kIndexType);

		VkViewport viewport{};
		viewport.width = (float)fbWidth;
		viewport.height = (float)fbHeight;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &viewport);

		// display rectangle [pos, pos + size] maps onto clip space [-1, 1]
		float transform[4];
		transform[0] = 2.0f / drawData->DisplaySize.x;
		transform[1] = 2.0f / drawData->DisplaySize.y;
		transform[2] = -1.0f - drawData->DisplayPos.x * transform[0];
		transform[3] = -1.0f - drawData->DisplayPos.y * transform[1];
		vkCmdPushConstants(cmd, s_vk.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(transform), transform);
	}

	// Every draw list goes into one vertex and one index buffer, back to back.
	void UploadGeometry(const ImDrawData* drawData, const FrameGeometry& geometry)
	{
		ImDrawVert* vertexOut = nullptr;
		ImDrawIdx* indexOut = nullptr;
		Report(vkMapMemory(s_vk.info.Device, geometry.vertices.memory, 0, VK_WHOLE_SIZE, 0, (void**)&vertexOut));
		Report(vkMapMemory(s_vk.info.Device, geometry.indices.memory, 0, VK_WHOLE_SIZE, 0, (void**)&indexOut));
		for (int list = 0; list < drawData->CmdListsCount; ++list)
		{
			const ImDrawList* dl = drawData->CmdLists[list];
			memcpy(vertexOut, dl->VtxBuffer.Data, (size_t)dl->VtxBuffer.Size * sizeof(ImDrawVert));
			memcpy(indexOut, dl->IdxBuffer.Data, (size_t)dl->IdxBuffer.Size * sizeof(ImDrawIdx));
			vertexOut += dl->VtxBuffer.Size;
			indexOut += dl->IdxBuffer.Size;
		}
		VkMappedMemoryRange ranges[2]{};
		ranges[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		ranges[0].memory = geometry.vertices.memory;
		ranges[0].size = VK_WHOLE_SIZE;
		ranges[1].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		ranges[1].memory = geometry.indices.memory;
		ranges[1].size = VK_WHOLE_SIZE;
		Report(vkFlushMappedMemoryRanges(s_vk.info.Device, 2, ranges));
		vkUnmapMemory(s_vk.info.Device, geometry.vertices.memory);
		vkUnmapMemory(s_vk.info.Device, geometry.indices.memory);
	}
}

bool ImGui_ImplVulkan_Init(ImGui_ImplVulkan_InitInfo* info, VkRenderPass render_pass)
{
	IM_ASSERT(info->Instance != VK_NULL_HANDLE && info->PhysicalDevice != VK_NULL_HANDLE);
	IM_ASSERT(info->Device != VK_NULL_HANDLE && info->Queue != VK_NULL_HANDLE);
	IM_ASSERT(info->DescriptorPool != VK_NULL_HANDLE);
	IM_ASSERT(info->MinImageCount >= 2 && info->ImageCount >= info->MinImageCount);
	IM_ASSERT(render_pass != VK_NULL_HANDLE);
	ImGui::GetIO().BackendRendererName = "pixelu_vulkan";
	s_vk.info = *info;
	s_vk.renderPass = render_pass;
	CreateDeviceObjects();
	return true;
}

void ImGui_ImplVulkan_Shutdown()
{
	DestroyDeviceObjects();
}

void ImGui_ImplVulkan_NewFrame(VkCommandBuffer command_buffer, VkFramebuffer framebuffer, VkExtent2D extent)
{
	// the overlay pass loads the game image, so there is nothing to clear
	VkRenderPassBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = s_vk.renderPass;
	begin.framebuffer = framebuffer;
	begin.renderArea.extent = extent;
	vkCmdBeginRenderPass(command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

void ImGui_ImplVulkan_RenderDrawData(ImDrawData* draw_data, VkCommandBuffer command_buffer)
{
	const int fbWidth = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
	const int fbHeight = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
	if (fbWidth <= 0 || fbHeight <= 0 || draw_data->TotalVtxCount == 0)
		return;

	if (s_vk.frames.empty())
	{
		s_vk.frames.resize(s_vk.info.ImageCount);
		s_vk.frameCursor = 0;
	}
	s_vk.frameCursor = (s_vk.frameCursor + 1) % s_vk.frames.size();
	FrameGeometry& geometry = s_vk.frames[s_vk.frameCursor];

	const size_t vertexBytes = (size_t)draw_data->TotalVtxCount * sizeof(ImDrawVert);
	const size_t indexBytes = (size_t)draw_data->TotalIdxCount * sizeof(ImDrawIdx);
	cemu_assert_debug(vertexBytes != 0 && indexBytes != 0);
	EnsureCapacity(geometry.vertices, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	EnsureCapacity(geometry.indices, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	UploadGeometry(draw_data, geometry);
	BindOverlayState(draw_data, command_buffer, geometry, fbWidth, fbHeight);

	// ==== vkdraw begin ====
	// Clip rectangles are in display space; shift and scale them into framebuffer pixels.
	const ImVec2 origin = draw_data->DisplayPos;
	const ImVec2 scale = draw_data->FramebufferScale;
	uint32 listFirstIndex = 0;
	sint32 listFirstVertex = 0;
	for (int list = 0; list < draw_data->CmdListsCount; ++list)
	{
		const ImDrawList* dl = draw_data->CmdLists[list];
		for (const ImDrawCmd& dc : dl->CmdBuffer)
		{
			if (dc.UserCallback)
			{
				if (dc.UserCallback == ImDrawCallback_ResetRenderState)
					BindOverlayState(draw_data, command_buffer, geometry, fbWidth, fbHeight);
				else
					dc.UserCallback(dl, &dc);
				continue;
			}
			float left = (dc.ClipRect.x - origin.x) * scale.x;
			float top = (dc.ClipRect.y - origin.y) * scale.y;
			const float right = (dc.ClipRect.z - origin.x) * scale.x;
			const float bottom = (dc.ClipRect.w - origin.y) * scale.y;
			const bool offscreen = left >= (float)fbWidth || top >= (float)fbHeight || right < 0.0f || bottom < 0.0f;
			if (offscreen)
				continue;
			if (left < 0.0f)
				left = 0.0f;
			if (top < 0.0f)
				top = 0.0f;
			VkRect2D scissor;
			scissor.offset.x = (int32_t)left;
			scissor.offset.y = (int32_t)top;
			scissor.extent.width = (uint32_t)(right - left);
			scissor.extent.height = (uint32_t)(bottom - top);
			vkCmdSetScissor(command_buffer, 0, 1, &scissor);
			const OverlayTexture* texture = (const OverlayTexture*)dc.TextureId;
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipelineLayout, 0, 1, &texture->set, 0, nullptr);
			vkCmdDrawIndexed(command_buffer, dc.ElemCount, 1, listFirstIndex + dc.IdxOffset, listFirstVertex + (sint32)dc.VtxOffset, 0);
		}
		listFirstIndex += (uint32)dl->IdxBuffer.Size;
		listFirstVertex += dl->VtxBuffer.Size;
	}
	// ==== vkdraw end ====
}

bool ImGui_ImplVulkan_CreateFontsTexture(VkCommandBuffer command_buffer)
{
	if (s_vk.fontView != VK_NULL_HANDLE)
		return true;
	ImGuiIO& io = ImGui::GetIO();
	unsigned char* pixels = nullptr;
	int width = 0, height = 0;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	const size_t bytes = (size_t)width * height * 4;

	CreateSampledImage((uint32)width, (uint32)height, s_vk.fontImage, s_vk.fontMemory, s_vk.fontView);
	const VkDescriptorSet set = (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(s_vk.fontSampler, s_vk.fontView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	const VkDeviceSize alignment = CreateHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, s_vk.fontStaging, s_vk.fontStagingMemory);
	if (alignment > s_vk.sizeGranularity)
		s_vk.sizeGranularity = alignment;
	cemu_assert_debug(bytes != 0);
	CopyToHostMemory(s_vk.fontStagingMemory, pixels, bytes);
	RecordImageUpload(command_buffer, s_vk.fontStaging, s_vk.fontImage, (uint32)width, (uint32)height);

	OverlayTexture* atlas = new OverlayTexture{};
	atlas->set = set;
	io.Fonts->TexID = (ImTextureID)atlas;
	return true;
}

void ImGui_ImplVulkan_DestroyFontUploadObjects()
{
	if (s_vk.fontStaging != VK_NULL_HANDLE)
		vkDestroyBuffer(s_vk.info.Device, s_vk.fontStaging, s_vk.info.Allocator);
	if (s_vk.fontStagingMemory != VK_NULL_HANDLE)
		vkFreeMemory(s_vk.info.Device, s_vk.fontStagingMemory, s_vk.info.Allocator);
	s_vk.fontStaging = VK_NULL_HANDLE;
	s_vk.fontStagingMemory = VK_NULL_HANDLE;
}

void ImGui_ImplVulkan_DestroyFontsTexture()
{
	ImGui_ImplVulkan_DestroyFontUploadObjects();
	DestroyFontImage();
	ImGuiIO& io = ImGui::GetIO();
	if (io.Fonts->TexID != (ImTextureID)nullptr)
	{
		// the atlas record only owns its descriptor set; everything else in it is null
		ImGui_ImplVulkan_DeleteTexture(io.Fonts->TexID);
		delete (OverlayTexture*)io.Fonts->TexID;
		io.Fonts->TexID = nullptr;
	}
}

void ImGui_ImplVulkan_SetMinImageCount(uint32_t min_image_count)
{
	IM_ASSERT(min_image_count >= 2);
	if (s_vk.info.MinImageCount == min_image_count)
		return;
	Report(vkDeviceWaitIdle(s_vk.info.Device));
	ReleaseAllGeometry();
	s_vk.info.MinImageCount = min_image_count;
}

ImTextureID ImGui_ImplVulkan_AddTexture(VkSampler sampler, VkImageView image_view, VkImageLayout image_layout)
{
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool = s_vk.info.DescriptorPool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &s_vk.setLayout;
	VkDescriptorSet set = VK_NULL_HANDLE;
	Report(vkAllocateDescriptorSets(s_vk.info.Device, &alloc, &set));

	VkDescriptorImageInfo image{};
	image.sampler = sampler;
	image.imageView = image_view;
	image.imageLayout = image_layout;
	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image;
	vkUpdateDescriptorSets(s_vk.info.Device, 1, &write, 0, nullptr);
	return (ImTextureID)set;
}

ImTextureID ImGui_ImplVulkan_GenerateTexture(VkCommandBuffer commandBuffer, const std::vector<uint8>& data, const Vector2i& size)
{
	try
	{
		OverlayTexture* texture = new OverlayTexture{};
		CreateSampledImage((uint32)size.x, (uint32)size.y, texture->image, texture->imageMemory, texture->view);
		texture->sampler = CreateOverlaySampler();
		texture->set = (VkDescriptorSet)ImGui_ImplVulkan_AddTexture(texture->sampler, texture->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		// the staging buffer stays with the texture until DeleteTexture
		CreateHostBuffer(data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, texture->staging, texture->stagingMemory);
		cemu_assert_debug(!data.empty());
		CopyToHostMemory(texture->stagingMemory, data.data(), data.size());
		RecordImageUpload(commandBuffer, texture->staging, texture->image, (uint32)size.x, (uint32)size.y);
		return (ImTextureID)texture;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't generate imgui texture: {}", ex.what());
		return nullptr;
	}
}

// Releases everything the texture owns and clears the record; the record itself stays
// allocated (the font path deletes it, other callers just drop the id).
void ImGui_ImplVulkan_DeleteTexture(ImTextureID id)
{
	OverlayTexture& texture = *(OverlayTexture*)id;
	const VkDevice device = s_vk.info.Device;
	vkFreeDescriptorSets(device, s_vk.info.DescriptorPool, 1, &texture.set);
	vkDestroySampler(device, texture.sampler, nullptr);
	vkDestroyImageView(device, texture.view, nullptr);
	vkDestroyImage(device, texture.image, nullptr);
	vkDestroyBuffer(device, texture.staging, nullptr);
	vkFreeMemory(device, texture.imageMemory, nullptr);
	vkFreeMemory(device, texture.stagingMemory, nullptr);
	texture = OverlayTexture{};
}
