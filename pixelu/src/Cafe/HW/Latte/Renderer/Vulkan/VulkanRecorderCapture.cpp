// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// PixelU recorder: asynchronous readback of the TV picture for LatteRecorder
// Works like HandleScreenshotRequest, but the copy is recorded into the current command buffer and
// collected a few frames later once the GPU is done with it, instead of waiting for the GPU.

#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"

void VulkanRenderer::recorder_pollFrames()
{
	// hand over finished readbacks in picture order
	std::array<RecorderReadback*, std::tuple_size_v<decltype(m_recorderReadbacks)>> finished;
	size_t finishedCount = 0;
	for (auto& readback : m_recorderReadbacks)
	{
		if (readback.pending && HasCommandBufferFinished(readback.commandBufferId))
			finished[finishedCount++] = &readback;
	}
	std::sort(finished.begin(), finished.begin() + finishedCount, [](const RecorderReadback* a, const RecorderReadback* b) { return a->frameSlot < b->frameSlot; });
	for (size_t i = 0; i < finishedCount; i++)
	{
		RecorderReadback& readback = *finished[i];
		readback.pending = false;
		const size_t size = (size_t)readback.width * readback.height * 4;
		std::vector<uint8> data = LatteRecorder::AcquireFrameBuffer(size);
		memcpy(data.data(), readback.mappedPtr, size);
		LatteRecorder::SubmitVideoFrame(std::move(data), readback.width, readback.height, readback.frameSlot, readback.colorConversion);
	}
}

void VulkanRenderer::recorder_waitForPendingReadbacks()
{
	bool anyPending = false;
	uint64 lastCommandBufferId = 0;
	for (auto& readback : m_recorderReadbacks)
	{
		if (!readback.pending)
			continue;
		anyPending = true;
		lastCommandBufferId = std::max(lastCommandBufferId, readback.commandBufferId);
	}
	if (!anyPending)
		return;
	WaitCommandBufferFinished(lastCommandBufferId);
	recorder_pollFrames();
}

bool VulkanRenderer::recorder_prepareReadbackBuffer(RecorderReadback& readback, size_t size)
{
	if (readback.buffer != VK_NULL_HANDLE && readback.bufferSize >= size)
		return true;
	if (readback.buffer != VK_NULL_HANDLE)
	{
		vkUnmapMemory(m_logicalDevice, readback.memory);
		memoryManager->DeleteBuffer(readback.buffer, readback.memory);
		readback.buffer = VK_NULL_HANDLE;
		readback.memory = VK_NULL_HANDLE;
		readback.mappedPtr = nullptr;
		readback.bufferSize = 0;
	}
	// cached memory makes the CPU-side copy fast, fall back to uncached if the device has none
	if (!memoryManager->CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, readback.buffer, readback.memory) &&
		!memoryManager->CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, readback.buffer, readback.memory))
	{
		readback.buffer = VK_NULL_HANDLE;
		readback.memory = VK_NULL_HANDLE;
		cemuLog_log(LogType::Force, "Recorder: failed to create a readback buffer");
		return false;
	}
	void* ptr = nullptr;
	if (vkMapMemory(m_logicalDevice, readback.memory, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS)
	{
		memoryManager->DeleteBuffer(readback.buffer, readback.memory);
		readback.buffer = VK_NULL_HANDLE;
		readback.memory = VK_NULL_HANDLE;
		return false;
	}
	readback.mappedPtr = (uint8*)ptr;
	readback.bufferSize = size;
	return true;
}

bool VulkanRenderer::recorder_prepareBlitImage(VkFormat format, uint32 width, uint32 height)
{
	if (m_recorderBlitImage != VK_NULL_HANDLE && m_recorderBlitFormat == format && m_recorderBlitWidth == width && m_recorderBlitHeight == height)
		return true;
	// the old image may still be read by a readback in flight
	recorder_waitForPendingReadbacks();
	if (m_recorderBlitImage != VK_NULL_HANDLE)
	{
		vkDestroyImage(m_logicalDevice, m_recorderBlitImage, nullptr);
		m_recorderBlitImage = VK_NULL_HANDLE;
	}
	if (m_recorderBlitImageMemory != VK_NULL_HANDLE)
	{
		vkFreeMemory(m_logicalDevice, m_recorderBlitImageMemory, nullptr);
		m_recorderBlitImageMemory = VK_NULL_HANDLE;
	}
	m_recorderBlitFormat = VK_FORMAT_UNDEFINED;

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.format = format;
	imageInfo.extent = {width, height, 1};
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.arrayLayers = 1;
	imageInfo.mipLevels = 1;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	if (vkCreateImage(m_logicalDevice, &imageInfo, nullptr, &m_recorderBlitImage) != VK_SUCCESS)
	{
		m_recorderBlitImage = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(m_logicalDevice, m_recorderBlitImage, &memRequirements);
	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	bool allocated = memoryManager->FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, allocInfo.memoryTypeIndex);
	if (allocated)
		allocated = vkAllocateMemory(m_logicalDevice, &allocInfo, nullptr, &m_recorderBlitImageMemory) == VK_SUCCESS;
	if (!allocated)
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
	for (auto& readback : m_recorderReadbacks)
	{
		if (readback.buffer != VK_NULL_HANDLE)
		{
			vkUnmapMemory(m_logicalDevice, readback.memory);
			memoryManager->DeleteBuffer(readback.buffer, readback.memory);
		}
		readback = {};
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
	auto texViewVk = (LatteTextureViewVk*)texView;
	LatteTextureVk* baseImageTex = texViewVk->GetBaseImage();
	if (texViewVk->firstMip != 0 || baseImageTex->dim == Latte::E_DIM::DIM_3D)
	{
		LatteRecorder::NotifyFrameDropped("unsupported TV buffer layout");
		return;
	}

	RecorderReadback* readback = nullptr;
	for (auto& it : m_recorderReadbacks)
	{
		if (!it.pending)
		{
			readback = &it;
			break;
		}
	}
	if (!readback)
	{
		LatteRecorder::NotifyFrameDropped("GPU readback still busy");
		return;
	}

	sint32 width, height;
	baseImageTex->GetEffectiveSize(width, height, 0);
	if (width <= 0 || height <= 0)
		return;

	// the output shader samples through the view: sRGB views are decoded and the TV buffer setting decides about encoding
	const VkFormat imageFormat = baseImageTex->GetFormat();
	const bool dstUsesSRGB = LatteGPUState.tvBufferUsesSRGB;
	bool directCopy = imageFormat == VK_FORMAT_R8G8B8A8_UNORM || imageFormat == VK_FORMAT_R8G8B8A8_SRGB;
	LatteRecorder::ColorConversion colorConversion = LatteRecorder::ColorConversion::None;
	VkFormat blitFormat = VK_FORMAT_UNDEFINED;
	if (directCopy)
	{
		const bool srcUsesSRGB = texViewVk->GetFormat() == VK_FORMAT_R8G8B8A8_SRGB;
		if (srcUsesSRGB && !dstUsesSRGB)
			colorConversion = LatteRecorder::ColorConversion::SRGBToLinear;
		else if (!srcUsesSRGB && dstUsesSRGB)
			colorConversion = LatteRecorder::ColorConversion::LinearToSRGB;
	}
	else
	{
		// other formats (10-bit, float, BGRA...) are converted by blitting into an RGBA8 image, the blit also handles sRGB
		blitFormat = dstUsesSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		VkFormatProperties formatProps;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, imageFormat, &formatProps);
		bool supportsBlit = (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, blitFormat, &formatProps);
		supportsBlit &= (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
		if (!supportsBlit)
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

	const size_t size = (size_t)width * height * 4;
	if (!recorder_prepareReadbackBuffer(*readback, size))
	{
		LatteRecorder::NotifyFrameDropped("failed to create readback buffer");
		return;
	}

	draw_endRenderPass();
	baseImageTex->GetImageObj()->flagForCurrentCommandBuffer();

	VkImageSubresourceLayers srcLayers{};
	srcLayers.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	srcLayers.mipLevel = 0;
	srcLayers.baseArrayLayer = texViewVk->firstSlice;
	srcLayers.layerCount = 1;

	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	region.bufferRowLength = (uint32)width;
	region.bufferImageHeight = (uint32)height;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = 0;
	region.imageSubresource.layerCount = 1;
	region.imageOffset = {0, 0, 0};
	region.imageExtent = {(uint32)width, (uint32)height, 1};

	if (directCopy)
	{
		barrier_image<ANY_TRANSFER | IMAGE_WRITE, TRANSFER_READ>(baseImageTex, srcLayers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		region.imageSubresource.baseArrayLayer = texViewVk->firstSlice;
		vkCmdCopyImageToBuffer(m_state.currentCommandBuffer, baseImageTex->GetImageObj()->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &region);
		barrier_image<TRANSFER_READ, ANY_TRANSFER | IMAGE_WRITE | IMAGE_READ>(baseImageTex, srcLayers, baseImageTex->GetDefaultLayout());
	}
	else
	{
		VkImageSubresourceRange blitRange{};
		blitRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blitRange.baseMipLevel = 0;
		blitRange.levelCount = 1;
		blitRange.baseArrayLayer = 0;
		blitRange.layerCount = 1;
		// waits for the previous readback of the same image to finish reading it
		barrier_image<TRANSFER_READ, TRANSFER_WRITE>(m_recorderBlitImage, blitRange, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		barrier_image<ANY_TRANSFER | IMAGE_WRITE, TRANSFER_READ>(baseImageTex, srcLayers, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		VkImageBlit blitRegion{};
		blitRegion.srcSubresource = srcLayers;
		blitRegion.srcOffsets[1] = {width, height, 1};
		blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blitRegion.dstSubresource.mipLevel = 0;
		blitRegion.dstSubresource.baseArrayLayer = 0;
		blitRegion.dstSubresource.layerCount = 1;
		blitRegion.dstOffsets[1] = {width, height, 1};
		vkCmdBlitImage(m_state.currentCommandBuffer, baseImageTex->GetImageObj()->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_recorderBlitImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitRegion, VK_FILTER_NEAREST);

		barrier_image<TRANSFER_READ, ANY_TRANSFER | IMAGE_WRITE | IMAGE_READ>(baseImageTex, srcLayers, baseImageTex->GetDefaultLayout());
		barrier_image<TRANSFER_WRITE, TRANSFER_READ>(m_recorderBlitImage, blitRange, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		region.imageSubresource.baseArrayLayer = 0;
		vkCmdCopyImageToBuffer(m_state.currentCommandBuffer, m_recorderBlitImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &region);
	}
	barrier_bufferRange<TRANSFER_WRITE, HOST_READ>(readback->buffer, 0, size);

	readback->pending = true;
	readback->commandBufferId = GetCurrentCommandBufferId();
	readback->frameSlot = frameSlot;
	readback->width = (uint32)width;
	readback->height = (uint32)height;
	readback->colorConversion = colorConversion;
	// the swap that follows submits the command buffer anyway, this keeps the latency low when it doesn't
	RequestSubmitSoon();
}
