// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanTextureReadback.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"

LatteTextureReadbackInfoVk::LatteTextureReadbackInfoVk(VkDevice device, LatteTextureView* textureView)
	: LatteTextureReadbackInfo(textureView, textureView->firstMip), m_device(device)
{
	m_image_size = GetImageSize(textureView);
	m_rowPitch = m_image_size / textureView->baseTexture->GetMipHeight(m_firstMip);
}

LatteTextureReadbackInfoVk::~LatteTextureReadbackInfoVk() = default;

uint32 LatteTextureReadbackInfoVk::GetImageSize(LatteTextureView* textureView)
{
	// converted host formats and block compressed data cannot be handed back as guest data
	auto* texture = (LatteTextureVk*)textureView->baseTexture;
	if (texture->m_isAlternateFormat || texture->IsCompressedFormat())
	{
		cemuLog_logDebug(LogType::Force, "Vulkan does not support readback of texture format 0x{:x}", (uint32)texture->format);
		return 0;
	}
	return LatteTextureReadbackInfo::GetReadbackImageSize(textureView, GetReadbackRowPitch(textureView));
}

void LatteTextureReadbackInfoVk::StartTransfer()
{
	cemu_assert(m_textureView);
	auto* texture = (LatteTextureVk*)m_textureView->baseTexture;
	cemu_assert_debug(texture->dim != Latte::E_DIM::DIM_3D);
	texture->GetImageObj()->flagForCurrentCommandBuffer();

	const uint32 width = texture->GetMipWidth(m_firstMip);
	const uint32 height = texture->GetMipHeight(m_firstMip);
	VkBufferImageCopy copy{};
	copy.bufferOffset = m_offset;
	copy.bufferRowLength = width;
	copy.bufferImageHeight = height;
	copy.imageSubresource.aspectMask = texture->GetImageAspect();
	copy.imageSubresource.mipLevel = m_firstMip;
	copy.imageSubresource.baseArrayLayer = m_firstSlice;
	copy.imageSubresource.layerCount = 1;
	copy.imageOffset = { 0, 0, 0 };
	copy.imageExtent = { width, height, 1 };

	VulkanRenderer* renderer = VulkanRenderer::GetInstance();
	renderer->draw_endRenderPass();

	// earlier writes finish, then the image is copied out in transfer source layout
	renderer->barrier_image<VulkanRenderer::ANY_TRANSFER | VulkanRenderer::IMAGE_WRITE, VulkanRenderer::TRANSFER_READ>(texture, copy.imageSubresource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	renderer->barrier_sequentializeTransfer();
	vkCmdCopyImageToBuffer(renderer->getCurrentCommandBuffer(), texture->GetImageObj()->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_target, 1, &copy);
	renderer->barrier_sequentializeTransfer();
	// later writes to the image wait for the copy, the host reads the buffer only after it
	renderer->barrier_image<VulkanRenderer::TRANSFER_READ, VulkanRenderer::ANY_TRANSFER | VulkanRenderer::IMAGE_WRITE>(texture, copy.imageSubresource, texture->GetDefaultLayout());
	renderer->barrier_bufferRange<VulkanRenderer::TRANSFER_WRITE, VulkanRenderer::HOST_READ>(m_target, m_offset, m_image_size);

	m_commandBufferId = renderer->GetCurrentCommandBufferId();
	m_textureView = nullptr;

	// get the command buffer submitted early so the result arrives sooner
	renderer->RequestSubmitSoon();
	renderer->RequestSubmitOnIdle();
}

bool LatteTextureReadbackInfoVk::IsFinished()
{
	return VulkanRenderer::GetInstance()->HasCommandBufferFinished(m_commandBufferId);
}

void LatteTextureReadbackInfoVk::ForceFinish()
{
	VulkanRenderer::GetInstance()->WaitCommandBufferFinished(m_commandBufferId);
}
