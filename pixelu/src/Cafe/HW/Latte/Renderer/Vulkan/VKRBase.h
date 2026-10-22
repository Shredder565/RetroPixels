// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "util/math/vector2.h"

// Indirection cell for an object that can be moved: holders of a reference keep the cell,
// and moving the object only repoints the cell.
class VKRMoveableRefCounterRef
{
public:
	class VKRMoveableRefCounter* ref;
};

// Objects reference each other to stay alive (a framebuffer holds its views and render pass).
// m_refCount counts incoming references; dropping the last one calls RefCountReachedZero().
class VKRMoveableRefCounter
{
public:
	VKRMoveableRefCounter() : m_handle(new VKRMoveableRefCounterRef())
	{
		m_handle->ref = this;
	}

	virtual ~VKRMoveableRefCounter()
	{
		cemu_assert_debug(m_refCount == 0);
#ifdef CEMU_DEBUG_ASSERT
		for (VKRMoveableRefCounterRef* target : m_targets)
		{
			auto& referrers = target->ref->m_debugReferrers;
			referrers.erase(std::remove(referrers.begin(), referrers.end(), m_handle), referrers.end());
		}
#endif
		// release outgoing references in the order they were taken
		for (VKRMoveableRefCounterRef* target : m_targets)
			target->ref->decRef();
		m_targets.clear();
		delete m_handle;
		m_handle = nullptr;
	}

	VKRMoveableRefCounter(const VKRMoveableRefCounter&) = delete;
	VKRMoveableRefCounter& operator=(const VKRMoveableRefCounter&) = delete;

	VKRMoveableRefCounter(VKRMoveableRefCounter&& other) noexcept
		: m_handle(other.m_handle), m_targets(std::move(other.m_targets))
	{
		m_refCount.store(other.m_refCount);
		other.m_refCount = 0;
		other.m_handle = nullptr;
		m_handle->ref = this;
	}

	VKRMoveableRefCounter& operator=(VKRMoveableRefCounter&& other) noexcept
	{
		cemu_assert(false); // not supported
		return *this;
	}

	// keep refTarget alive at least as long as this object
	void addRef(VKRMoveableRefCounter* refTarget)
	{
		m_targets.emplace_back(refTarget->m_handle);
		refTarget->m_refCount++;
#ifdef CEMU_DEBUG_ASSERT
		refTarget->m_debugReferrers.emplace_back(m_handle);
#endif
	}

	// references held by something that is not itself a ref counted object
	void incRef()
	{
		m_refCount++;
	}

	void decRef()
	{
		if (--m_refCount == 0)
			RefCountReachedZero();
	}

protected:
	virtual void RefCountReachedZero()
	{
	}

	std::atomic_int_least32_t m_refCount{};

private:
	VKRMoveableRefCounterRef* m_handle;
	std::vector<VKRMoveableRefCounterRef*> m_targets; // objects this one keeps alive
#ifdef CEMU_DEBUG_ASSERT
	std::vector<VKRMoveableRefCounterRef*> m_debugReferrers; // objects keeping this one alive
#endif
};

// a Vulkan object whose destruction waits until no submitted command buffer uses it
class VKRDestructibleObject : public VKRMoveableRefCounter
{
public:
	void flagForCurrentCommandBuffer();
	bool canDestroy();

	virtual ~VKRDestructibleObject() {}

private:
	uint64 m_lastCmdBufferId{};
};

class VKRObjectTexture : public VKRDestructibleObject
{
public:
	VKRObjectTexture();
	~VKRObjectTexture() override;

	VkImage m_image{ VK_NULL_HANDLE };
	VkFormat m_format = VK_FORMAT_UNDEFINED;
	VkImageCreateFlags m_flags;
	VkImageAspectFlags m_imageAspect;
	struct VkImageMemAllocation* m_allocation{};
};

class VKRObjectTextureView : public VKRDestructibleObject
{
public:
	VKRObjectTextureView(VKRObjectTexture* tex, VkImageView view);
	~VKRObjectTextureView() override;

	VkImageView m_textureImageView{ VK_NULL_HANDLE };
	VkSampler m_textureDefaultSampler[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE }; // nearest and linear samplers owned by the view
};

// samplers are shared through a cache keyed by their creation parameters
class VKRObjectSampler : public VKRDestructibleObject
{
public:
	VKRObjectSampler(VkSamplerCreateInfo* samplerInfo);
	~VKRObjectSampler() override;

	static VKRObjectSampler* GetOrCreateSampler(VkSamplerCreateInfo* samplerInfo);
	static void DestroyCache();

	void RefCountReachedZero() override; // released once nothing references it

	VkSampler GetSampler() const { return m_sampler; }

private:
	static std::unordered_map<uint64, VKRObjectSampler*> s_samplerCache;
	VkSampler m_sampler{ VK_NULL_HANDLE };
	uint64 m_hash;
};

class VKRObjectRenderPass : public VKRDestructibleObject
{
public:
	// an attachment slot is used when isPresent is set or a view is given
	struct AttachmentEntryColor_t
	{
		VKRObjectTextureView* viewObj{};
		bool isPresent{};
		VkFormat format;
	};

	struct AttachmentEntryDepth_t
	{
		VKRObjectTextureView* viewObj{};
		bool isPresent{};
		VkFormat format;
		bool hasStencil;
	};

	struct AttachmentInfo_t
	{
		AttachmentEntryColor_t colorAttachment[Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS];
		AttachmentEntryDepth_t depthAttachment;
	};

	VkFormat GetColorFormat(size_t index)
	{
		return index < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS ? m_colorAttachmentFormat[index] : VK_FORMAT_UNDEFINED;
	}

	VkFormat GetDepthFormat()
	{
		return m_depthAttachmentFormat;
	}

	VKRObjectRenderPass(AttachmentInfo_t& attachmentInfo, sint32 colorAttachmentCount = Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS);
	~VKRObjectRenderPass() override;

	VkRenderPass m_renderPass{ VK_NULL_HANDLE };
	VkFormat m_colorAttachmentFormat[Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS];
	VkFormat m_depthAttachmentFormat;
	uint64 m_hashForPipeline; // summary of the attachment formats, part of the pipeline state hash
};

class VKRObjectFramebuffer : public VKRDestructibleObject
{
public:
	VKRObjectFramebuffer(VKRObjectRenderPass* renderPass, std::span<VKRObjectTextureView*> attachments, Vector2i size);
	~VKRObjectFramebuffer() override;

	VkFramebuffer m_frameBuffer{ VK_NULL_HANDLE };
};

class VKRObjectPipeline : public VKRDestructibleObject
{
public:
	VKRObjectPipeline();
	~VKRObjectPipeline() override;

	// a pipeline is assigned once, possibly from a compile thread
	void SetPipeline(VkPipeline newPipeline);
	VkPipeline GetPipeline() const { return m_pipeline; }

	VkDescriptorSetLayout m_vertexDSL = VK_NULL_HANDLE, m_pixelDSL = VK_NULL_HANDLE, m_geometryDSL = VK_NULL_HANDLE;
	VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

private:
	VkPipeline m_pipeline = VK_NULL_HANDLE;
};

class VKRObjectDescriptorSet : public VKRDestructibleObject
{
public:
	VKRObjectDescriptorSet();
	~VKRObjectDescriptorSet() override;

	VkDescriptorSet descriptorSet{ VK_NULL_HANDLE };
};
