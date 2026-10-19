// Original code for RetroPixels (PixelU). Not derived from Cemu.

// Recorder support: the TV picture is copied into a host visible buffer inside the current command
// buffer, and handed to LatteRecorder once that command buffer has finished (checked every frame),
// so capturing never stalls the GPU. Formats other than RGBA8 are converted by a blit first.

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"

namespace
{
	constexpr VkMemoryPropertyFlags kHostReadable = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	bool IsRGBA8(VkFormat f)
	{
		return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB;
	}

	VkImageSubresourceLayers ColorLayer(uint32 layer)
	{
		VkImageSubresourceLayers l{};
		l.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		l.mipLevel = 0;
		l.baseArrayLayer = layer;
		l.layerCount = 1;
		return l;
	}
}

void VulkanRenderer::recorder_pollFrames()
{
	// collect every finished readback, then submit them ordered by frame slot
	std::array<RecorderReadback*, std::tuple_size_v<decltype(m_recorderReadbacks)>> done;
	size_t doneCount = 0;
	for (RecorderReadback& rb : m_recorderReadbacks)
	{
		if (rb.pending && HasCommandBufferFinished(rb.commandBufferId))
			done[doneCount++] = &rb;
	}
	std::sort(done.begin(), done.begin() + doneCount, [](const RecorderReadback* x, const RecorderReadback* y) { return x->frameSlot < y->frameSlot; });
	for (size_t n = 0; n < doneCount; n++)
	{
		RecorderReadback& rb = *done[n];
		rb.pending = false;
		const size_t bytes = (size_t)rb.width * rb.height * 4;
		std::vector<uint8> frame = LatteRecorder::AcquireFrameBuffer(bytes);
		memcpy(frame.data(), rb.mappedPtr, bytes);
		LatteRecorder::SubmitVideoFrame(std::move(frame), rb.width, rb.height, rb.frameSlot, rb.colorConversion);
	}
}

void VulkanRenderer::recorder_waitForPendingReadbacks()
{
	// waiting for the newest pending command buffer covers all older ones
	std::optional<uint64> newest;
	for (const RecorderReadback& rb : m_recorderReadbacks)
	{
		if (rb.pending)
			newest = std::max(newest.value_or(0), rb.commandBufferId);
	}
	if (!newest)
		return;
	WaitCommandBufferFinished(*newest);
	recorder_pollFrames();
}

bool VulkanRenderer::recorder_prepareReadbackBuffer(RecorderReadback& readback, size_t size)
{
	if (readback.buffer != VK_NULL_HANDLE)
	{
		if (readback.bufferSize >= size)
			return true;
		// too small, replace it
		vkUnmapMemory(m_logicalDevice, readback.memory);
		memoryManager->DeleteBuffer(readback.buffer, readback.memory);
		readback.buffer = VK_NULL_HANDLE;
		readback.memory = VK_NULL_HANDLE;
		readback.mappedPtr = nullptr;
		readback.bufferSize = 0;
	}

	// prefer cached memory, the CPU copies out of it every frame
	const bool created =
		memoryManager->CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHostReadable | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, readback.buffer, readback.memory) ||
		memoryManager->CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHostReadable, readback.buffer, readback.memory);
	if (!created)
	{
		readback.buffer = VK_NULL_HANDLE;
		readback.memory = VK_NULL_HANDLE;
		cemuLog_log(LogType::Force, "Recorder: failed to create a readback buffer");
		return false;
	}

	void* mapped = nullptr;
	if (vkMapMemory(m_logicalDevice, readback.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
	{
		memoryManager->DeleteBuffer(readback.buffer, readback.memory);
		readback.buffer = VK_NULL_HANDLE;
		readback.memory = VK_NULL_HANDLE;
		return false;
	}
	readback.mappedPtr = (uint8*)mapped;
	readback.bufferSize = size;
	return true;
}

bool VulkanRenderer::recorder_prepareBlitImage(VkFormat format, uint32 width, uint32 height)
{
	const bool matches = m_recorderBlitImage != VK_NULL_HANDLE && m_recorderBlitFormat == format && m_recorderBlitWidth == width && m_recorderBlitHeight == height;
	if (matches)
		return true;

	// a readback in flight may still copy out of the current image
	recorder_waitForPendingReadbacks();
	if (m_recorderBlitImage != VK_NULL_HANDLE)
		vkDestroyImage(m_logicalDevice, m_recorderBlitImage, nullptr);
	if (m_recorderBlitImageMemory != VK_NULL_HANDLE)
		vkFreeMemory(m_logicalDevice, m_recorderBlitImageMemory, nullptr);
	m_recorderBlitImage = VK_NULL_HANDLE;
	m_recorderBlitImageMemory = VK_NULL_HANDLE;
	m_recorderBlitFormat = VK_FORMAT_UNDEFINED;

	VkImageCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = { width, height, 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(m_logicalDevice, &info, nullptr, &m_recorderBlitImage) != VK_SUCCESS)
	{
		m_recorderBlitImage = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(m_logicalDevice, m_recorderBlitImage, &req);
	VkMemoryAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	const bool haveMemory = memoryManager->FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, alloc.memoryTypeIndex) &&
		vkAllocateMemory(m_logicalDevice, &alloc, nullptr, &m_recorderBlitImageMemory) == VK_SUCCESS;
	if (!haveMemory)
	{
		vkDestroyImage(m_logicalDevice, m_recorderBlitImage, nullptr);
		m_recorderBlitImage = VK_NULL_HANDLE;
		m_recorderBlitImageMemory = VK_NULL_HANDLE;
		return false;
	}
	vkBindImageMemory(m_logicalDevice, m_recorderBlitImage, m_recorderBlitImageMemory, 0);
	m_recorderBlitFormat = format;
	m_recorderBlitWidth = width;
	m_recorderBlitHeight = height;
	return true;
}

void VulkanRenderer::recorder_releaseResources()
{
	for (RecorderReadback& rb : m_recorderReadbacks)
	{
		if (rb.buffer != VK_NULL_HANDLE)
		{
			vkUnmapMemory(m_logicalDevice, rb.memory);
			memoryManager->DeleteBuffer(rb.buffer, rb.memory);
		}
		rb = {};
	}
	if (m_recorderBlitImage != VK_NULL_HANDLE)
		vkDestroyImage(m_logicalDevice, m_recorderBlitImage, nullptr);
	if (m_recorderBlitImageMemory != VK_NULL_HANDLE)
		vkFreeMemory(m_logicalDevice, m_recorderBlitImageMemory, nullptr);
	m_recorderBlitImage = VK_NULL_HANDLE;
	m_recorderBlitImageMemory = VK_NULL_HANDLE;
	m_recorderBlitFormat = VK_FORMAT_UNDEFINED;
}

void VulkanRenderer::recorder_captureTVFrame(LatteTextureView* texView, sint64 frameSlot)
{
	auto* view = (LatteTextureViewVk*)texView;
	LatteTextureVk* image = view->GetBaseImage();
	if (view->firstMip != 0 || image->dim == Latte::E_DIM::DIM_3D)
	{
		LatteRecorder::NotifyFrameDropped("unsupported TV buffer layout");
		return;
	}

	// the first idle readback slot takes this frame
	auto slotIt = std::find_if(m_recorderReadbacks.begin(), m_recorderReadbacks.end(), [](const RecorderReadback& rb) { return !rb.pending; });
	if (slotIt == m_recorderReadbacks.end())
	{
		LatteRecorder::NotifyFrameDropped("GPU readback still busy");
		return;
	}
	RecorderReadback& slot = *slotIt;

	sint32 width, height;
	image->GetEffectiveSize(width, height, 0);
	if (width <= 0 || height <= 0)
		return;

	// The presented picture goes through the view (an sRGB view decodes) and gets encoded according
	// to the TV buffer setting. RGBA8 is copied as is and the recorder applies that difference;
	// anything else is blitted into an RGBA8 image of the TV buffer's encoding.
	const VkFormat imageFormat = image->GetFormat();
	const bool tvIsSRGB = LatteGPUState.tvBufferUsesSRGB;
	const bool needsBlit = !IsRGBA8(imageFormat);
	LatteRecorder::ColorConversion conversion = LatteRecorder::ColorConversion::None;
	if (!needsBlit)
	{
		const bool viewIsSRGB = view->GetFormat() == VK_FORMAT_R8G8B8A8_SRGB;
		if (viewIsSRGB != tvIsSRGB)
			conversion = viewIsSRGB ? LatteRecorder::ColorConversion::SRGBToLinear : LatteRecorder::ColorConversion::LinearToSRGB;
	}
	else
	{
		const VkFormat blitFormat = tvIsSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, imageFormat, &props);
		const bool canSource = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, blitFormat, &props);
		const bool canTarget = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
		if (!canSource || !canTarget)
		{
			LatteRecorder::NotifyFrameDropped("TV buffer format cannot be converted");
			return;
		}
		if (!recorder_prepareBlitImage(blitFormat, (uint32)width, (uint32)height))
		{
			LatteRecorder::NotifyFrameDropped("failed to create conversion image");
			return;
		}
	}

	const size_t bytes = (size_t)width * height * 4;
	if (!recorder_prepareReadbackBuffer(slot, bytes))
	{
		LatteRecorder::NotifyFrameDropped("failed to create readback buffer");
		return;
	}

	draw_endRenderPass();
	image->GetImageObj()->flagForCurrentCommandBuffer();
	VkCommandBuffer cmd = m_state.currentCommandBuffer;
	VkImageSubresourceLayers tvLayer = ColorLayer(view->firstSlice);

	// tightly packed RGBA8 rows, copied from whichever image ends up holding RGBA8
	VkBufferImageCopy toBuffer{};
	toBuffer.bufferOffset = 0;
	toBuffer.bufferRowLength = (uint32)width;
	toBuffer.bufferImageHeight = (uint32)height;
	toBuffer.imageOffset = { 0, 0, 0 };
	toBuffer.imageExtent = { (uint32)width, (uint32)height, 1 };

	if (!needsBlit)
	{
		barrier_image<ANY_TRANSFER | IMAGE_WRITE, TRANSFER_READ>(image, tvLayer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		toBuffer.imageSubresource = ColorLayer(view->firstSlice);
		vkCmdCopyImageToBuffer(cmd, image->GetImageObj()->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer, 1, &toBuffer);
		barrier_image<TRANSFER_READ, ANY_TRANSFER | IMAGE_WRITE | IMAGE_READ>(image, tvLayer, image->GetDefaultLayout());
	}
	else
	{
		VkImageSubresourceRange blitRange{};
		blitRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blitRange.baseMipLevel = 0;
		blitRange.levelCount = 1;
		blitRange.baseArrayLayer = 0;
		blitRange.layerCount = 1;
		// the previous readback has to be done reading the conversion image before it is overwritten
		barrier_image<TRANSFER_READ, TRANSFER_WRITE>(m_recorderBlitImage, blitRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		barrier_image<ANY_TRANSFER | IMAGE_WRITE, TRANSFER_READ>(image, tvLayer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		VkImageBlit blit{};
		blit.srcSubresource = tvLayer;
		blit.srcOffsets[1] = { width, height, 1 };
		blit.dstSubresource = ColorLayer(0);
		blit.dstOffsets[1] = { width, height, 1 };
		vkCmdBlitImage(cmd, image->GetImageObj()->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_recorderBlitImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

		barrier_image<TRANSFER_READ, ANY_TRANSFER | IMAGE_WRITE | IMAGE_READ>(image, tvLayer, image->GetDefaultLayout());
		barrier_image<TRANSFER_WRITE, TRANSFER_READ>(m_recorderBlitImage, blitRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		toBuffer.imageSubresource = ColorLayer(0);
		vkCmdCopyImageToBuffer(cmd, m_recorderBlitImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer, 1, &toBuffer);
	}
	barrier_bufferRange<TRANSFER_WRITE, HOST_READ>(slot.buffer, 0, bytes);

	slot.pending = true;
	slot.commandBufferId = GetCurrentCommandBufferId();
	slot.frameSlot = frameSlot;
	slot.width = (uint32)width;
	slot.height = (uint32)height;
	slot.colorConversion = conversion;
	// normally the following swap submits anyway; this keeps latency low when it does not
	RequestSubmitSoon();
}
