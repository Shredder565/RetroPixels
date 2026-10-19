// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"

namespace
{
	// Some guest formats store their channels in a different place than the host format they are
	// mapped to; the sampler's component selects (0-3 = RGBA, 4/5 = 0/1) are remapped to compensate.
	uint32 RemapComponentSelect(Latte::E_GX2SURFFMT format, uint32 sel)
	{
		using Fmt = Latte::E_GX2SURFFMT;
		const bool isChannel = sel <= 3;
		switch (format)
		{
		case Fmt::R8_UNORM:
		case Fmt::R8_SNORM:
		case Fmt::BC4_UNORM:
		case Fmt::BC4_SNORM:
			// single channel formats return red in every channel
			return (sel >= 1 && sel <= 3) ? 0 : sel;
		case Fmt::A1_B5_G5_R5_UNORM:
		case Fmt::A2_B10_G10_R10_UNORM:
			// channel order is reversed relative to the host format
			return isChannel ? 3 - sel : sel;
		case Fmt::BC5_UNORM:
		case Fmt::BC5_SNORM:
			// two channel format, alpha reads green
			return sel == 3 ? 1 : sel;
		case Fmt::X24_G8_UINT:
			// only the stencil part is meaningful, it lives in alpha
			return isChannel ? 3 : sel;
		case Fmt::R4_G4_UNORM:
			// red and green trade places
			if (sel == 0)
				return 1;
			if (sel == 1)
				return 0;
			return sel;
		default:
			return sel;
		}
	}

	VkImageViewType ViewTypeOf(Latte::E_DIM dim)
	{
		switch (dim)
		{
		case Latte::E_DIM::DIM_1D:
			return VK_IMAGE_VIEW_TYPE_1D;
		case Latte::E_DIM::DIM_2D:
		case Latte::E_DIM::DIM_2D_MSAA:
			return VK_IMAGE_VIEW_TYPE_2D;
		case Latte::E_DIM::DIM_2D_ARRAY:
			return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		case Latte::E_DIM::DIM_3D:
			return VK_IMAGE_VIEW_TYPE_3D;
		case Latte::E_DIM::DIM_CUBEMAP:
			return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
		default:
			cemu_assert_unimplemented();
			return VK_IMAGE_VIEW_TYPE_2D;
		}
	}

	// the 3-bit component select of the sampler word, 6 and 7 behave like 4 (zero)
	constexpr VkComponentSwizzle s_componentSources[8] = {
		VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A,
		VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO
	};

	constexpr uint32 kSwizzleMask = 0x0FFF0000; // the four component selects of SQ_TEX_RESOURCE word 4
	constexpr uint32 kSwizzleRGBA = 0x06880000; // 0,1,2,3
}

LatteTextureViewVk::LatteTextureViewVk(VkDevice device, LatteTextureVk* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
	: LatteTextureView(texture, firstMip, mipCount, firstSlice, sliceCount, dim, format), m_device(device)
{
	const bool sameAsTexture = dim == texture->dim && format == texture->format;
	if (texture->overwriteInfo.hasFormatOverwrite)
	{
		// textures with an overridden format do not alias, so their views keep the texture's format
		cemu_assert_debug(format == texture->format);
		m_format = texture->GetFormat();
	}
	else if (sameAsTexture)
	{
		m_format = texture->GetFormat();
	}
	else
	{
		VulkanRenderer::FormatInfoVK info;
		VulkanRenderer::GetInstance()->GetTextureFormatInfoVK(format, texture->isDepth, dim, 0, 0, &info);
		m_format = info.vkImageFormat;
	}
	m_uniqueId = VulkanRenderer::GetInstance()->GenUniqueId();
}

LatteTextureViewVk::~LatteTextureViewVk()
{
	// a descriptor set info unregisters itself when deleted
	while (!m_descriptorSets.empty())
		delete m_descriptorSets.front();

	VulkanRenderer* renderer = VulkanRenderer::GetInstance();
	for (CachedView& slot : m_inlineViews)
	{
		if (slot.view)
			renderer->ReleaseDestructibleObject(slot.view);
	}
	if (m_moreViews)
	{
		for (auto& entry : *m_moreViews)
			renderer->ReleaseDestructibleObject(entry.second);
		delete m_moreViews;
		m_moreViews = nullptr;
	}
}

VKRObjectTextureView* LatteTextureViewVk::MakeImageView(uint32 swizzle)
{
	LatteTextureVk* image = GetBaseImage();

	VkImageViewCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.image = image->GetImageObj()->m_image;
	info.viewType = ViewTypeOf(dim);
	info.format = m_format;

	VkComponentSwizzle* targets[4] = { &info.components.r, &info.components.g, &info.components.b, &info.components.a };
	for (uint32 c = 0; c < 4; c++)
	{
		const uint32 sel = (swizzle >> (16 + c * 3)) & 7;
		*targets[c] = s_componentSources[RemapComponentSelect(format, sel)];
	}

	// depth/stencil images are only sampled for depth
	VkImageAspectFlags aspect = image->GetImageAspect();
	if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
		aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
	info.subresourceRange.aspectMask = aspect;
	info.subresourceRange.baseMipLevel = firstMip;
	info.subresourceRange.levelCount = numMip;
	if (info.viewType == VK_IMAGE_VIEW_TYPE_3D && baseTexture->Is3DTexture())
	{
		// a 3D view of a 3D image always covers all slices
		cemu_assert_debug(firstMip == 0);
		cemu_assert_debug(numSlice == baseTexture->depth);
		info.subresourceRange.baseArrayLayer = 0;
		info.subresourceRange.layerCount = 1;
	}
	else
	{
		info.subresourceRange.baseArrayLayer = firstSlice;
		info.subresourceRange.layerCount = numSlice;
	}

	VkImageView handle;
	if (vkCreateImageView(m_device, &info, nullptr, &handle) != VK_SUCCESS)
		throw std::runtime_error("failed to create texture image view!");
	return new VKRObjectTextureView(image->GetImageObj(), handle);
}

VKRObjectTextureView* LatteTextureViewVk::GetViewRGBA()
{
	return GetSamplerView(kSwizzleRGBA);
}

VKRObjectTextureView* LatteTextureViewVk::GetSamplerView(uint32 gpuSamplerSwizzle)
{
	const uint32 swizzle = gpuSamplerSwizzle & kSwizzleMask;
	if (m_inlineViews[0].swizzle == swizzle)
		return m_inlineViews[0].view;
	if (m_inlineViews[1].swizzle == swizzle)
		return m_inlineViews[1].view;
	if (m_moreViews)
	{
		auto it = m_moreViews->find(swizzle);
		if (it != m_moreViews->end())
			return it->second;
	}

	VKRObjectTextureView* view = MakeImageView(swizzle);
	for (CachedView& slot : m_inlineViews)
	{
		if (slot.swizzle == kFreeSlot)
		{
			slot.swizzle = swizzle;
			slot.view = view;
			return view;
		}
	}
	if (!m_moreViews)
		m_moreViews = new std::unordered_map<uint32, VKRObjectTextureView*>();
	(*m_moreViews)[swizzle] = view;
	return view;
}

VkSampler LatteTextureViewVk::GetDefaultTextureSampler(bool useLinearTexFilter)
{
	// the RGBA image view owns the two default samplers (nearest, linear)
	VkSampler& sampler = GetViewRGBA()->m_textureDefaultSampler[useLinearTexFilter ? 1 : 0];
	if (sampler != VK_NULL_HANDLE)
		return sampler;

	const VkFilter filter = useLinearTexFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
	VkSamplerCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter = filter;
	info.minFilter = filter;
	// a max LOD of 0.25 keeps sampling on mip 0 while magnification/minification still follow
	// the LOD, which is how OpenGL's non-mipmapped min filters behave
	info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.minLod = 0.0f;
	info.maxLod = 0.25f;
	info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vkCreateSampler(m_device, &info, nullptr, &sampler) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to create default sampler");
		throw std::runtime_error("failed to create texture sampler!");
	}
	return sampler;
}
