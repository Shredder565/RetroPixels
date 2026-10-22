// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "util/ChunkedHeap/ChunkedHeap.h"

#include "Cafe/HW/Latte/Renderer/Vulkan/VKRBase.h"

// A guest texture backed by a VkImage. The image layout is tracked per subresource: one entry per
// mip level for 3D textures, one per mip level and array layer otherwise.
class LatteTextureVk : public LatteTexture
{
public:
	LatteTextureVk(class VulkanRenderer* vkRenderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels,
		uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth);

	~LatteTextureVk();

	void AllocateOnHost() override;

	VKRObjectTexture* GetImageObj() const { return vkObjTex; };

	VkFormat GetFormat() const { return vkObjTex->m_format; }
	VkImageAspectFlags GetImageAspect() const { return vkObjTex->m_imageAspect; }
	VkImageLayout GetDefaultLayout() const { return m_defaultLayout; }

	VkImageLayout GetImageLayout(VkImageSubresource& subresource)
	{
		return m_layouts[LayoutSlot(subresource.mipLevel, subresource.arrayLayer)];
	}

	// a range covers a single mip level; all layers in it are expected to share one layout
	VkImageLayout GetImageLayout(VkImageSubresourceRange& subresource)
	{
		cemu_assert_debug(subresource.levelCount == 1);
		const size_t first = LayoutSlot(subresource.baseMipLevel, subresource.baseArrayLayer);
		if (!Is3DTexture())
		{
			cemu_assert_debug(subresource.layerCount > 0);
			for (uint32 layer = 1; layer < subresource.layerCount; layer++)
				cemu_assert_debug(m_layouts[first + layer] == m_layouts[first]);
		}
		return m_layouts[first];
	}

	void SetImageLayout(VkImageSubresource& subresource, VkImageLayout newLayout)
	{
		m_layouts[LayoutSlot(subresource.mipLevel, subresource.arrayLayer)] = newLayout;
	}

	void SetImageLayout(VkImageSubresourceRange& subresource, VkImageLayout newLayout)
	{
		cemu_assert_debug(subresource.levelCount == 1);
		const size_t first = LayoutSlot(subresource.baseMipLevel, subresource.baseArrayLayer);
		const uint32 layers = Is3DTexture() ? 1 : subresource.layerCount;
		for (uint32 layer = 0; layer < layers; layer++)
			m_layouts[first + layer] = newLayout;
	}

protected:
	LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) override;

public:
	uint64 m_vkFlushIndex{}; // used to track read-write dependencies within the same renderpass

	uint64 m_vkFlushIndex_read{};
	uint64 m_vkFlushIndex_write{};

	uint32 m_selfDependencyCheckIndex{}; // draw in which the texture was last seen; detects sampling from a bound render target
	VkImageAspectFlags m_selfDependencyCheckAspectMask{};

	bool m_isAlternateFormat{}; // the host format differs from the guest format (data gets converted)
private:
	size_t LayoutSlot(uint32 mip, uint32 layer) const
	{
		cemu_assert_debug(mip < m_layoutsMips);
		cemu_assert_debug(layer < m_layoutsDepth);
		return Is3DTexture() ? mip : (size_t)mip * m_layoutsDepth + layer;
	}

	class VulkanRenderer* m_vkr{};

	VKRObjectTexture* vkObjTex{};
	// long term layout: GENERAL, or ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT for render targets when that is supported
	VkImageLayout m_defaultLayout{ VK_IMAGE_LAYOUT_GENERAL };
	std::vector<VkImageLayout> m_layouts;
	uint32 m_layoutsMips;
	uint32 m_layoutsDepth;
};
