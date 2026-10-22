// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VKRBase.h"

// Render target combination of the Vulkan backend: a render pass plus framebuffer, and the same
// attachments described for VK_KHR_dynamic_rendering.
class CachedFBOVk : public LatteCachedFBO
{
public:
	CachedFBOVk(uint64 key, VkDevice device)
		: LatteCachedFBO(key), m_device(device)
	{
		BuildRenderPass();
		BuildFramebuffer();
		BuildRenderingInfo();
	}

	~CachedFBOVk();

	static inline FSpinlock s_spinlockDependency;

	VKRObjectFramebuffer* GetFramebufferObj() const
	{
		return m_vkrObjFramebuffer;
	}

	VKRObjectRenderPass* GetRenderPassObj() const
	{
		return m_vkrObjRenderPass;
	}

	// for KHR_dynamic_rendering
	VkRenderingInfoKHR* GetRenderingInfo()
	{
		return &m_renderingInfo;
	}

	// pipelines compiled against this render pass; they are deleted with it
	void TrackDependency(class PipelineInfo* pipelineInfo)
	{
		s_spinlockDependency.lock();
		m_usedByPipelines.emplace_back(pipelineInfo);
		s_spinlockDependency.unlock();
	}

	void RemoveDependency(class PipelineInfo* pipelineInfo)
	{
		s_spinlockDependency.lock();
		vectorRemoveByValue(m_usedByPipelines, pipelineInfo);
		s_spinlockDependency.unlock();
	}

	[[nodiscard]] const VkExtent2D& GetExtend() const { return m_extent; }

	struct RendertargetSelfDependencyMask
	{
		VkImageAspectFlags aspectMaskFlags{}; // aspects that are sampled and rendered to in the same draw
		bool hasNonPixelSelfDependency{false};

		VkImageAspectFlags GetAspectMask() const
		{
			return aspectMaskFlags;
		}

		bool HasSelfDependency() const
		{
			return GetAspectMask() != 0;
		}

		// a vertex or geometry shader samples one of the render targets
		bool HasVertexOrGeometrySelfDependency() const
		{
			return hasNonPixelSelfDependency;
		}
	};

	// which of the textures bound by the three descriptor sets are also attachments of this FBO
	RendertargetSelfDependencyMask CheckForSelfDependency(VkDescriptorSetInfo* vsDS, VkDescriptorSetInfo* gsDS, VkDescriptorSetInfo* psDS) const;

private:
	void BuildRenderPass();
	void BuildFramebuffer();
	void BuildRenderingInfo();

	VKRObjectTextureView* ColorView(uint32 index);
	VKRObjectTextureView* DepthView(bool& hasStencil);

	VkDevice m_device;
	VKRObjectRenderPass* m_vkrObjRenderPass{};
	VKRObjectFramebuffer* m_vkrObjFramebuffer{};

	VkExtent2D m_extent;

	// for KHR_dynamic_rendering
	VkRenderingInfoKHR m_renderingInfo{};
	VkRenderingAttachmentInfoKHR m_colorAttachments[8]{};
	VkRenderingAttachmentInfoKHR m_depthAttachment{};
	VkRenderingAttachmentInfoKHR m_stencilAttachment{};

	std::vector<class PipelineInfo*> m_usedByPipelines;
};
