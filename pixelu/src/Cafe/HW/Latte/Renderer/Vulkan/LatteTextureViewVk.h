// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VKRBase.h"

// A guest texture view. The Vulkan image views are made on demand, one per sampler swizzle.
class LatteTextureViewVk : public LatteTextureView
{
public:
	LatteTextureViewVk(VkDevice device, class LatteTextureVk* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount);
	~LatteTextureViewVk();

	uint64 GetUniqueId() const { return m_uniqueId; };
	VKRObjectTextureView* GetViewRGBA();
	VKRObjectTextureView* GetSamplerView(uint32 gpuSamplerSwizzle);
	VkSampler GetDefaultTextureSampler(bool useLinearTexFilter);
	VkFormat GetFormat() const { return m_format; }

	LatteTextureVk* GetBaseImage() const { return (LatteTextureVk*)baseTexture; }

	// descriptor sets that use this view; they are deleted together with the view
	void AddDescriptorSetReference(struct VkDescriptorSetInfo* dsInfo)
	{
		if (std::find(m_descriptorSets.begin(), m_descriptorSets.end(), dsInfo) == m_descriptorSets.end())
			m_descriptorSets.emplace_back(dsInfo);
	};
	void RemoveDescriptorSetReference(struct VkDescriptorSetInfo* dsInfo)
	{
		m_descriptorSets.erase(std::remove(m_descriptorSets.begin(), m_descriptorSets.end(), dsInfo), m_descriptorSets.end());
	};

private:
	VKRObjectTextureView* MakeImageView(uint32 swizzle);

	// Most views only ever see one or two swizzles, so two inline slots come first and the map
	// is only allocated for the rest.
	static constexpr uint32 kFreeSlot = 0xFFFFFFFF;
	struct CachedView
	{
		uint32 swizzle = kFreeSlot;
		VKRObjectTextureView* view = nullptr;
	};
	CachedView m_inlineViews[2];
	std::unordered_map<uint32, VKRObjectTextureView*>* m_moreViews{};

	VkDevice m_device;
	VkFormat m_format;
	std::vector<struct VkDescriptorSetInfo*> m_descriptorSets;

	uint64 m_uniqueId;
};
