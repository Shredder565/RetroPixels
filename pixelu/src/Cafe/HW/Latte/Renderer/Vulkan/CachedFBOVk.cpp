// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/CachedFBOVk.h"

namespace
{
	// attachment that is neither loaded nor stored; filled in when the slot is used
	VkRenderingAttachmentInfoKHR UnusedAttachment()
	{
		VkRenderingAttachmentInfoKHR a{};
		a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
		a.imageView = VK_NULL_HANDLE;
		a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		a.resolveMode = VK_RESOLVE_MODE_NONE;
		a.resolveImageView = VK_NULL_HANDLE;
		a.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		a.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		return a;
	}

	LatteTextureVk* BaseTextureOf(LatteTextureView* view)
	{
		return static_cast<LatteTextureVk*>(view->baseTexture);
	}

	// increases with every self dependency check, marks the attachments of the FBO being checked
	uint32 s_checkGeneration = 1;
}

void CachedFBOVk::BuildRenderPass()
{
	VKRObjectRenderPass::AttachmentInfo_t attachments;
	for (uint32 i = 0; i < 8; i++)
	{
		auto& slot = attachments.colorAttachment[i];
		slot.viewObj = nullptr;
		auto* view = (LatteTextureViewVk*)colorBuffer[i].texture;
		if (!view)
			continue;
		slot.viewObj = view->GetViewRGBA();
		slot.format = view->GetFormat();
	}

	auto& depth = attachments.depthAttachment;
	depth.viewObj = nullptr;
	if (auto* view = static_cast<LatteTextureViewVk*>(depthBuffer.texture))
	{
		depth.viewObj = view->GetViewRGBA();
		depth.format = view->GetFormat();
		depth.hasStencil = view->baseTexture->hasStencil;
	}
	m_vkrObjRenderPass = new VKRObjectRenderPass(attachments);
}

CachedFBOVk::~CachedFBOVk()
{
	// a pipeline unregisters itself when deleted
	while (!m_usedByPipelines.empty())
		delete m_usedByPipelines.front();
	VulkanRenderer* renderer = VulkanRenderer::GetInstance();
	renderer->ReleaseDestructibleObject(m_vkrObjFramebuffer);
	m_vkrObjFramebuffer = nullptr;
	renderer->ReleaseDestructibleObject(m_vkrObjRenderPass);
	m_vkrObjRenderPass = nullptr;
}

VKRObjectTextureView* CachedFBOVk::ColorView(uint32 index)
{
	cemu_assert(index < 8);
	auto* view = (LatteTextureViewVk*)colorBuffer[index].texture;
	// slices of 3D textures are rendered through their RGBA view as well
	return view ? view->GetViewRGBA() : nullptr;
}

VKRObjectTextureView* CachedFBOVk::DepthView(bool& hasStencil)
{
	hasStencil = false;
	auto* view = (LatteTextureViewVk*)depthBuffer.texture;
	if (!view)
		return nullptr;
	cemu_assert_debug(view->numMip == 1);
	hasStencil = view->baseTexture->hasStencil;
	return view->GetViewRGBA();
}

void CachedFBOVk::BuildFramebuffer()
{
	// colour attachments in slot order without gaps, then depth/stencil
	std::array<VKRObjectTextureView*, 9> views{};
	size_t count = 0;
	for (uint32 i = 0; i < 8; i++)
	{
		if (VKRObjectTextureView* v = ColorView(i))
			views[count++] = v;
	}
	bool hasStencil;
	if (VKRObjectTextureView* v = DepthView(hasStencil))
		views[count++] = v;
	cemu_assert_debug(count < 9);

	m_vkrObjFramebuffer = new VKRObjectFramebuffer(m_vkrObjRenderPass, std::span<VKRObjectTextureView*>(views.data(), count), m_size);
	m_extent = { (uint32)m_size.x, (uint32)m_size.y };
}

void CachedFBOVk::BuildRenderingInfo()
{
	// colour slots keep their index; unused trailing slots are cut off
	uint32 colorCount = 0;
	for (uint32 i = 0; i < 8; i++)
	{
		VkRenderingAttachmentInfoKHR& a = m_colorAttachments[i];
		a = UnusedAttachment();
		a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VKRObjectTextureView* view = ColorView(i);
		if (!view)
			continue;
		a.imageView = view->m_textureImageView;
		a.imageLayout = BaseTextureOf(colorBuffer[i].texture)->GetDefaultLayout();
		colorCount = i + 1;
	}

	m_depthAttachment = UnusedAttachment();
	m_stencilAttachment = UnusedAttachment();
	m_renderingInfo.pDepthAttachment = nullptr;
	m_renderingInfo.pStencilAttachment = nullptr;
	bool hasStencil;
	if (VKRObjectTextureView* view = DepthView(hasStencil))
	{
		const VkImageLayout layout = BaseTextureOf(depthBuffer.texture)->GetDefaultLayout();
		m_depthAttachment.imageView = view->m_textureImageView;
		m_depthAttachment.imageLayout = layout;
		m_depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		m_depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		m_renderingInfo.pDepthAttachment = &m_depthAttachment;
		m_stencilAttachment.imageLayout = layout;
		if (hasStencil)
		{
			m_stencilAttachment.imageView = view->m_textureImageView;
			m_stencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			m_stencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			m_renderingInfo.pStencilAttachment = &m_stencilAttachment;
		}
	}

	m_renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
	m_renderingInfo.pNext = nullptr;
	m_renderingInfo.flags = 0;
	m_renderingInfo.renderArea.offset = { 0, 0 };
	m_renderingInfo.renderArea.extent = m_extent;
	m_renderingInfo.layerCount = 1;
	m_renderingInfo.viewMask = 0;
	m_renderingInfo.colorAttachmentCount = colorCount;
	m_renderingInfo.pColorAttachments = m_colorAttachments;
}

CachedFBOVk::RendertargetSelfDependencyMask CachedFBOVk::CheckForSelfDependency(VkDescriptorSetInfo* vsDS, VkDescriptorSetInfo* gsDS, VkDescriptorSetInfo* psDS) const
{
	// tag every attachment's texture with this check's generation and the aspects written
	const uint32 generation = ++s_checkGeneration;
	for (const auto& cb : colorBuffer)
	{
		if (!cb.texture)
			continue;
		LatteTextureVk* tex = BaseTextureOf(cb.texture);
		tex->m_selfDependencyCheckIndex = generation;
		tex->m_selfDependencyCheckAspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	}
	if (depthBuffer.texture)
	{
		LatteTextureVk* tex = BaseTextureOf(depthBuffer.texture);
		tex->m_selfDependencyCheckIndex = generation;
		tex->m_selfDependencyCheckAspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | (depthBuffer.hasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	}

	// then look for tagged textures among the sampled ones
	auto sampledAttachmentAspects = [generation](VkDescriptorSetInfo* ds) -> VkImageAspectFlags {
		VkImageAspectFlags aspects = 0;
		if (ds)
		{
			for (LatteTextureVk* tex : ds->list_fboCandidates)
			{
				if (tex->m_selfDependencyCheckIndex == generation)
					aspects |= tex->m_selfDependencyCheckAspectMask;
			}
		}
		return aspects;
	};
	const VkImageAspectFlags fromVertex = sampledAttachmentAspects(vsDS);
	const VkImageAspectFlags fromGeometry = sampledAttachmentAspects(gsDS);
	const VkImageAspectFlags fromPixel = sampledAttachmentAspects(psDS);

	RendertargetSelfDependencyMask result{};
	result.aspectMaskFlags = fromVertex | fromGeometry | fromPixel;
	result.hasNonPixelSelfDependency = (fromVertex | fromGeometry) != 0;
	return result;
}
