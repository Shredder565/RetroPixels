// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

namespace
{
	VkImageType ImageTypeOf(Latte::E_DIM dim)
	{
		switch (dim)
		{
		case Latte::E_DIM::DIM_1D:
			return VK_IMAGE_TYPE_1D;
		case Latte::E_DIM::DIM_3D:
			return VK_IMAGE_TYPE_3D;
		case Latte::E_DIM::DIM_2D:
		case Latte::E_DIM::DIM_2D_ARRAY:
		case Latte::E_DIM::DIM_CUBEMAP:
		case Latte::E_DIM::DIM_2D_MSAA:
			return VK_IMAGE_TYPE_2D;
		default:
			cemu_assert_unimplemented();
			return VK_IMAGE_TYPE_1D; // zero, like an unset field
		}
	}
}

LatteTextureVk::LatteTextureVk(class VulkanRenderer* vkRenderer, Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle,
	Latte::E_HWTILEMODE tileMode, bool isDepth)
	: LatteTexture(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth), m_vkr(vkRenderer)
{
	vkObjTex = new VKRObjectTexture();

	// graphic packs can override the resolution of the host image
	sint32 hostWidth = width, hostHeight = height, hostDepth = depth;
	if (overwriteInfo.hasResolutionOverwrite)
	{
		hostWidth = overwriteInfo.width;
		hostHeight = overwriteInfo.height;
		hostDepth = overwriteInfo.depth;
	}
	hostDepth = std::max(hostDepth, 1);

	VulkanRenderer::FormatInfoVK fmtInfo;
	vkRenderer->GetTextureFormatInfoVK(format, isDepth, dim, hostWidth, hostHeight, &fmtInfo);
	cemu_assert_debug(hasStencil == ((fmtInfo.vkImageAspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0));
	vkObjTex->m_imageAspect = fmtInfo.vkImageAspect;
	m_isAlternateFormat = fmtInfo.isAlternateFormat;

	VkImageCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = ImageTypeOf(dim);
	info.format = fmtInfo.vkImageFormat;
	info.extent.width = hostWidth;
	info.extent.height = hostHeight;
	info.mipLevels = mipLevels;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (dim == Latte::E_DIM::DIM_3D)
	{
		// slices of a 3D texture can be viewed as a 2D array (render to slice)
		info.extent.depth = hostDepth;
		info.arrayLayers = 1;
		info.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
	}
	else
	{
		info.extent.depth = 1;
		info.arrayLayers = hostDepth;
		const bool canBeCube = dim != Latte::E_DIM::DIM_1D && hostWidth == hostHeight && (hostDepth % 6) == 0;
		if (canBeCube)
			info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	}

	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (isDepth)
	{
		info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	}
	else
	{
		// colour images get reinterpreted through views of other formats
		info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
		if (fmtInfo.isCompressed)
			info.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
		// compressed images and R4G4 (not renderable in Vulkan) are never render targets
		if (!Latte::IsCompressedFormat(format) && fmtInfo.vkImageFormat != VK_FORMAT_R4G4_UNORM_PACK8)
			info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}
	const bool isAttachment = (info.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
	if (isAttachment && m_vkr->UseAttachmentFeedbackLoop())
	{
		info.usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
		m_defaultLayout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
	}

	VkDevice device = m_vkr->GetLogicalDevice();
	if (vkCreateImage(device, &info, nullptr, &vkObjTex->m_image) != VK_SUCCESS)
		m_vkr->UnrecoverableError("Failed to create texture image");
	if (m_vkr->IsDebugMarkersEnabled())
	{
		const std::string label = fmt::format("tex_{:08x}_fmt{:04x}_tm{:x}", physAddress, (uint32)format, (uint32)tileMode);
		VkDebugUtilsObjectNameInfoEXT nameInfo{};
		nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		nameInfo.objectType = VK_OBJECT_TYPE_IMAGE;
		nameInfo.objectHandle = (uint64_t)vkObjTex->m_image;
		nameInfo.pObjectName = label.c_str();
		vkSetDebugUtilsObjectNameEXT(device, &nameInfo);
	}
	vkObjTex->m_flags = info.flags;
	vkObjTex->m_format = info.format;

	// every subresource starts out undefined; the table uses the guest mip and slice counts
	m_layoutsMips = std::max(mipLevels, 1u);
	m_layoutsDepth = std::max(depth, 1u);
	m_layouts.assign(Is3DTexture() ? m_layoutsMips : m_layoutsMips * m_layoutsDepth, VK_IMAGE_LAYOUT_UNDEFINED);
}

LatteTextureVk::~LatteTextureVk()
{
	cemu_assert_debug(views.empty());
	m_vkr->surfaceCopy_notifyTextureRelease(this);
	VulkanRenderer::GetInstance()->ReleaseDestructibleObject(vkObjTex);
	vkObjTex = nullptr;
}

LatteTextureView* LatteTextureVk::CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
{
	cemu_assert_debug(mipCount > 0 && sliceCount > 0);
	cemu_assert_debug((firstMip + mipCount) <= this->mipLevels);
	cemu_assert_debug((firstSlice + sliceCount) <= this->depth);
	return new LatteTextureViewVk(m_vkr->GetLogicalDevice(), this, dim, format, firstMip, mipCount, firstSlice, sliceCount);
}

void LatteTextureVk::AllocateOnHost()
{
	vkObjTex->m_allocation = VulkanRenderer::GetInstance()->GetMemoryManager()->imageMemoryAllocate(vkObjTex->m_image);
}
