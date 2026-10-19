// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"

// Copies between colour and depth textures (which Vulkan cannot copy directly) are done by drawing a
// full screen quad that texel-fetches the source. One pipeline exists per destination format and
// copy direction; each keeps the framebuffers and descriptor sets made for it, per texture, slice and mip.

namespace
{
	struct CopyPushConstants
	{
		float vertexOffsets[4 * 2]; // quad corners in clip space
		sint32 srcTexelOffset[2];
	};

	// per texture storage indexed by the texture's slice/mip index
	template<typename T>
	class PerSubresource
	{
	public:
		explicit PerSubresource(LatteTextureVk* texture) : m_texture(texture) {}
		~PerSubresource()
		{
			for (T* entry : m_entries)
				delete entry;
		}
		PerSubresource(const PerSubresource&) = delete;
		PerSubresource& operator=(const PerSubresource&) = delete;

		T* Find(sint32 slice, sint32 mip) const
		{
			const size_t index = (size_t)m_texture->GetSliceMipArrayIndex(slice, mip);
			return index < m_entries.size() ? m_entries[index] : nullptr;
		}

		T* Add(sint32 slice, sint32 mip)
		{
			const size_t index = (size_t)m_texture->GetSliceMipArrayIndex(slice, mip);
			if (index >= m_entries.size())
				m_entries.resize(index + 1, nullptr);
			T* entry = new T();
			m_entries[index] = entry;
			return entry;
		}

		template<typename F>
		void ForEach(F&& f)
		{
			for (T* entry : m_entries)
			{
				if (entry)
					f(*entry);
			}
		}

	private:
		LatteTextureVk* m_texture;
		std::vector<T*> m_entries;
	};

	template<typename T>
	void Release(T*& obj)
	{
		if (obj)
			VulkanRenderer::GetInstance()->ReleaseDestructibleObject(obj);
		obj = nullptr;
	}

	// The GLSL text is part of what gets compiled, keep it unchanged.
	const char* const s_copyVertexShader =
		"#version 450\r\n"
		"layout(location = 0) out ivec2 passSrcTexelOffset;\r\n"
		"layout(push_constant) uniform pushConstants {\r\n"
		"vec2 vertexOffsets[4];\r\n"
		"ivec2 srcTexelOffset;\r\n"
		"}uf_pushConstants;\r\n"
		"\r\n"
		"void main(){\r\n"
		"vec2 tPOS;\r\n"
		"switch(gl_VertexIndex)"
		"{\r\n"
		// a switch instead of indexing the push constant array, which some AMD drivers get wrong
		"case 0: tPOS = uf_pushConstants.vertexOffsets[0].xy; break;\r\n"
		"case 1: tPOS = uf_pushConstants.vertexOffsets[1].xy; break;\r\n"
		"case 2: tPOS = uf_pushConstants.vertexOffsets[3].xy; break;\r\n"
		"case 3: tPOS = uf_pushConstants.vertexOffsets[0].xy; break;\r\n"
		"case 4: tPOS = uf_pushConstants.vertexOffsets[2].xy; break;\r\n"
		"case 5: tPOS = uf_pushConstants.vertexOffsets[3].xy; break;\r\n"
		"}"
		"passSrcTexelOffset = uf_pushConstants.srcTexelOffset;\r\n"
		"gl_Position = vec4(tPOS, 0, 1.0);\r\n"
		"}\r\n";

	const char* const s_copyColorToDepthShader =
		"#version 450\r\n"
		"layout(location = 0) in flat ivec2 passSrcTexelOffset;\r\n"
		"layout(binding = 0) uniform sampler2D textureSrc;\r\n"
		"in vec4 gl_FragCoord;\r\n"
		"\r\n"
		"void main(){\r\n"
		"gl_FragDepth = texelFetch(textureSrc, passSrcTexelOffset + ivec2(gl_FragCoord.xy), 0).r;\r\n"
		"}\r\n";

	const char* const s_copyDepthToColorShader =
		"#version 450\r\n"
		"layout(location = 0) in flat ivec2 passSrcTexelOffset;\r\n"
		"layout(binding = 0) uniform sampler2D textureSrc;\r\n"
		"layout(location = 0) out vec4 colorOut0;\r\n"
		"in vec4 gl_FragCoord;\r\n"
		"\r\n"
		"void main(){\r\n"
		"colorOut0.r = texelFetch(textureSrc, passSrcTexelOffset + ivec2(gl_FragCoord.xy), 0).r;\r\n"
		"}\r\n";

	RendererShaderVk* CompileInternalShader(RendererShader::ShaderType type, const char* source)
	{
		auto* shader = new RendererShaderVk(type, 0, 0, false, false, std::string(source));
		shader->PreponeCompilation(true);
		return shader;
	}
}

struct CopySurfacePipelineInfo
{
	struct FramebufferValue
	{
		VKRObjectFramebuffer* vkObjFramebuffer{};
		VKRObjectTextureView* vkObjImageView{};
	};

	struct DescriptorValue
	{
		VKRObjectDescriptorSet* vkObjDescriptorSet{};
		VKRObjectTextureView* vkObjImageView{};
	};

	CopySurfacePipelineInfo() = default;
	CopySurfacePipelineInfo(const CopySurfacePipelineInfo&) = delete;
	~CopySurfacePipelineInfo()
	{
		Release(vkObjRenderPass);
		Release(vkObjPipeline);
		for (auto& [texture, entries] : map_framebuffers)
			entries.ForEach([](FramebufferValue& v) { ReleaseFramebuffer(v); });
		for (auto& [texture, entries] : map_descriptors)
			entries.ForEach([](DescriptorValue& v) { ReleaseDescriptor(v); });
	}

	static void ReleaseFramebuffer(FramebufferValue& v)
	{
		Release(v.vkObjFramebuffer);
		Release(v.vkObjImageView);
	}

	static void ReleaseDescriptor(DescriptorValue& v)
	{
		Release(v.vkObjImageView);
		Release(v.vkObjDescriptorSet);
	}

	VKRObjectPipeline* vkObjPipeline{};
	VKRObjectRenderPass* vkObjRenderPass{};

	// keyed by destination texture
	std::unordered_map<LatteTextureVk*, PerSubresource<FramebufferValue>> map_framebuffers;
	// keyed by source texture
	std::unordered_map<LatteTextureVk*, PerSubresource<DescriptorValue>> map_descriptors;
};

struct VkCopySurfaceState_t
{
	LatteTextureVk* sourceTexture;
	sint32 srcMip;
	sint32 srcSlice;
	LatteTextureVk* destinationTexture;
	sint32 dstMip;
	sint32 dstSlice;
	sint32 width;
	sint32 height;
};

extern std::atomic_int g_compiling_pipelines;

uint64 VulkanRenderer::copySurface_getPipelineStateHash(VkCopySurfaceState_t& state)
{
	// everything the pipeline depends on: destination format and which side is depth
	uint64 key = (uint64)(uint32)state.destinationTexture->GetFormat();
	if (state.sourceTexture->isDepth)
		key |= 1ull << 32;
	if (state.destinationTexture->isDepth)
		key |= 1ull << 33;
	return key;
}

CopySurfacePipelineInfo* VulkanRenderer::copySurface_getCachedPipeline(VkCopySurfaceState_t& state)
{
	auto it = m_copySurfacePipelineCache.find(copySurface_getPipelineStateHash(state));
	return it != m_copySurfacePipelineCache.end() ? it->second : nullptr;
}

VKRObjectRenderPass* VulkanRenderer::copySurface_createRenderpass(VkCopySurfaceState_t& state)
{
	// a single attachment, the destination texture
	LatteTextureVk* dst = state.destinationTexture;
	VKRObjectTextureView* view = ((LatteTextureViewVk*)dst->baseView)->GetViewRGBA();
	VKRObjectRenderPass::AttachmentInfo_t attachments{};
	if (dst->isDepth)
	{
		attachments.depthAttachment.viewObj = view;
		attachments.depthAttachment.format = dst->GetFormat();
		attachments.depthAttachment.hasStencil = dst->hasStencil;
	}
	else
	{
		attachments.colorAttachment[0].viewObj = view;
		attachments.colorAttachment[0].format = dst->GetFormat();
	}
	return new VKRObjectRenderPass(attachments, 1);
}

CopySurfacePipelineInfo* VulkanRenderer::copySurface_getOrCreateGraphicsPipeline(VkCopySurfaceState_t& state)
{
	if (CopySurfacePipelineInfo* cached = copySurface_getCachedPipeline(state))
		return cached;

	if (!defaultShaders.copySurface_vs)
	{
		defaultShaders.copySurface_vs = CompileInternalShader(RendererShader::ShaderType::kVertex, s_copyVertexShader);
		defaultShaders.copySurface_psColor2Depth = CompileInternalShader(RendererShader::ShaderType::kFragment, s_copyColorToDepthShader);
		defaultShaders.copySurface_psDepth2Color = CompileInternalShader(RendererShader::ShaderType::kFragment, s_copyDepthToColorShader);
	}

	const bool srcDepth = state.sourceTexture->isDepth;
	const bool dstDepth = state.destinationTexture->isDepth;
	cemu_assert(srcDepth != dstDepth); // only colour <-> depth goes through here
	RendererShaderVk* fragmentShader = srcDepth ? defaultShaders.copySurface_psDepth2Color : defaultShaders.copySurface_psColor2Depth;
	const VkPipelineShaderStageCreateInfo stages[2] = {
		CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, defaultShaders.copySurface_vs->GetShaderModule(), "main"),
		CreatePipelineShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, fragmentShader->GetShaderModule(), "main"),
	};

	// registered right away; a pipeline that failed to build stays cached as it is
	auto* info = new CopySurfacePipelineInfo();
	m_copySurfacePipelineCache.try_emplace(copySurface_getPipelineStateHash(state), info);
	auto* pipelineObj = new VKRObjectPipeline();

	// set 0: the source texture
	VkDescriptorSetLayoutBinding sourceBinding{};
	sourceBinding.binding = 0;
	sourceBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sourceBinding.descriptorCount = 1;
	sourceBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
	setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutInfo.bindingCount = 1;
	setLayoutInfo.pBindings = &sourceBinding;
	if (vkCreateDescriptorSetLayout(m_logicalDevice, &setLayoutInfo, nullptr, &pipelineObj->m_pixelDSL) != VK_SUCCESS)
		UnrecoverableError("Failed to create descriptor set layout for surface copy shader");

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pushRange.offset = 0;
	pushRange.size = sizeof(CopyPushConstants);
	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &pipelineObj->m_pixelDSL;
	layoutInfo.pushConstantRangeCount = 1;
	layoutInfo.pPushConstantRanges = &pushRange;
	VkResult res = vkCreatePipelineLayout(m_logicalDevice, &layoutInfo, nullptr, &pipelineObj->m_pipelineLayout);
	if (res != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to create pipeline layout: {}", res);
		pipelineObj->SetPipeline(VK_NULL_HANDLE);
		return info;
	}

	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; // no vertex buffers, positions come from push constants

	VkPipelineInputAssemblyStateCreateInfo assembly{};
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	assembly.primitiveRestartEnable = VK_FALSE;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	raster.depthClampEnable = VK_FALSE;
	raster.rasterizerDiscardEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	multisample.sampleShadingEnable = VK_FALSE;

	// depth to colour writes the red channel only
	VkPipelineColorBlendAttachmentState blendAttachment{};
	blendAttachment.blendEnable = VK_FALSE;
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
	VkPipelineColorBlendStateCreateInfo blend{};
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.logicOpEnable = VK_FALSE;
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttachment;

	// colour to depth always writes depth
	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = dstDepth ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = dstDepth ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;
	depthStencil.minDepthBounds = 0.0f;
	depthStencil.maxDepthBounds = 1.0f;

	const VkDynamicState dynamicList[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic{};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = (uint32)std::size(dynamicList);
	dynamic.pDynamicStates = dynamicList;

	info->vkObjRenderPass = copySurface_createRenderpass(state);
	pipelineObj->addRef(info->vkObjRenderPass);
	info->vkObjPipeline = pipelineObj;

	VkGraphicsPipelineCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	createInfo.stageCount = (uint32)std::size(stages);
	createInfo.pStages = stages;
	createInfo.pVertexInputState = &vertexInput;
	createInfo.pInputAssemblyState = &assembly;
	createInfo.pViewportState = &viewportState;
	createInfo.pRasterizationState = &raster;
	createInfo.pMultisampleState = &multisample;
	createInfo.pDepthStencilState = &depthStencil;
	createInfo.pColorBlendState = dstDepth ? nullptr : &blend;
	createInfo.pDynamicState = &dynamic;
	createInfo.layout = pipelineObj->m_pipelineLayout;
	createInfo.renderPass = info->vkObjRenderPass->m_renderPass;
	createInfo.subpass = 0;

	VkPipeline pipeline = VK_NULL_HANDLE;
	res = vkCreateGraphicsPipelines(m_logicalDevice, m_pipeline_cache, 1, &createInfo, nullptr, &pipeline);
	if (res != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to create graphics pipeline for surface copy. Error {} Info:", (sint32)res);
		cemu_assert_suspicious();
		pipeline = VK_NULL_HANDLE;
	}
	pipelineObj->SetPipeline(pipeline);
	return info;
}

VKRObjectTextureView* VulkanRenderer::surfaceCopy_createImageView(LatteTextureVk* textureVk, uint32 sliceIndex, uint32 mipIndex)
{
	// a plain 2D view of exactly one slice and mip
	VkImageViewCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.image = textureVk->GetImageObj()->m_image;
	info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	info.format = textureVk->GetFormat();
	info.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
	info.subresourceRange.aspectMask = textureVk->isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	info.subresourceRange.baseMipLevel = mipIndex;
	info.subresourceRange.levelCount = 1;
	info.subresourceRange.baseArrayLayer = sliceIndex;
	info.subresourceRange.layerCount = 1;
	VkImageView view;
	if (vkCreateImageView(m_logicalDevice, &info, nullptr, &view) != VK_SUCCESS)
		UnrecoverableError("Failed to create framebuffer image view for copy surface operation");
	return new VKRObjectTextureView(textureVk->GetImageObj(), view);
}

VKRObjectFramebuffer* VulkanRenderer::surfaceCopy_getOrCreateFramebuffer(VkCopySurfaceState_t& state, CopySurfacePipelineInfo* pipelineInfo)
{
	LatteTextureVk* dst = state.destinationTexture;
	auto known = pipelineInfo->map_framebuffers.find(dst);
	if (known != pipelineInfo->map_framebuffers.end())
	{
		if (auto* entry = known->second.Find(state.dstSlice, state.dstMip))
			return entry->vkObjFramebuffer;
	}

	VKRObjectTextureView* view = surfaceCopy_createImageView(dst, state.dstSlice, state.dstMip);
	sint32 width, height;
	dst->GetEffectiveSize(width, height, state.dstMip);
	std::array<VKRObjectTextureView*, 1> attachments = { view };
	auto* framebuffer = new VKRObjectFramebuffer(pipelineInfo->vkObjRenderPass, attachments, Vector2i(width, height));

	auto* entry = pipelineInfo->map_framebuffers.try_emplace(dst, dst).first->second.Add(state.dstSlice, state.dstMip);
	entry->vkObjFramebuffer = framebuffer;
	entry->vkObjImageView = view;
	return framebuffer;
}

VKRObjectDescriptorSet* VulkanRenderer::surfaceCopy_getOrCreateDescriptorSet(VkCopySurfaceState_t& state, CopySurfacePipelineInfo* pipelineInfo)
{
	LatteTextureVk* src = state.sourceTexture;
	auto known = pipelineInfo->map_descriptors.find(src);
	if (known != pipelineInfo->map_descriptors.end())
	{
		if (auto* entry = known->second.Find(state.srcSlice, state.srcMip))
			return entry->vkObjDescriptorSet;
	}

	auto* setObj = new VKRObjectDescriptorSet();
	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &pipelineInfo->vkObjPipeline->m_pixelDSL;
	if (vkAllocateDescriptorSets(m_logicalDevice, &allocInfo, &setObj->descriptorSet) != VK_SUCCESS)
		UnrecoverableError("failed to allocate descriptor set for surface copy operation");

	VKRObjectTextureView* view = surfaceCopy_createImageView(src, state.srcSlice, state.srcMip);
	setObj->addRef(view);

	// the shader uses texelFetch, so the sampler settings hardly matter; the view owns it
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.mipLodBias = 0.0f;
	samplerInfo.anisotropyEnable = VK_FALSE;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 0.0f;
	samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	if (vkCreateSampler(m_logicalDevice, &samplerInfo, nullptr, &view->m_textureDefaultSampler[0]) != VK_SUCCESS)
		UnrecoverableError("Failed to create texture sampler for surface copy operation");

	VkDescriptorImageInfo imageInfo{};
	imageInfo.sampler = view->m_textureDefaultSampler[0];
	imageInfo.imageView = view->m_textureImageView;
	imageInfo.imageLayout = src->GetDefaultLayout();
	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = setObj->descriptorSet;
	write.dstBinding = 0;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageInfo;
	vkUpdateDescriptorSets(m_logicalDevice, 1, &write, 0, nullptr);

	auto* entry = pipelineInfo->map_descriptors.try_emplace(src, src).first->second.Add(state.srcSlice, state.srcMip);
	entry->vkObjDescriptorSet = setObj;
	entry->vkObjImageView = view;
	return setObj;
}

void VulkanRenderer::surfaceCopy_viaDrawcall(LatteTextureVk* srcTextureVk, sint32 texSrcMip, sint32 texSrcSlice, LatteTextureVk* dstTextureVk, sint32 texDstMip, sint32 texDstSlice, sint32 effectiveCopyWidth, sint32 effectiveCopyHeight)
{
	draw_endRenderPass();

	auto layersOf = [](LatteTextureVk* tex, sint32 mip, sint32 slice) {
		VkImageSubresourceLayers l;
		l.aspectMask = tex->GetImageAspect();
		l.mipLevel = mip;
		l.baseArrayLayer = slice;
		l.layerCount = 1;
		return l;
	};
	VkImageSubresourceLayers srcLayers = layersOf(srcTextureVk, texSrcMip, texSrcSlice);
	VkImageSubresourceLayers dstLayers = layersOf(dstTextureVk, texDstMip, texDstSlice);

	VkCopySurfaceState_t state;
	state.sourceTexture = srcTextureVk;
	state.srcMip = texSrcMip;
	state.srcSlice = texSrcSlice;
	state.destinationTexture = dstTextureVk;
	state.dstMip = texDstMip;
	state.dstSlice = texDstSlice;
	state.width = effectiveCopyWidth;
	state.height = effectiveCopyHeight;

	CopySurfacePipelineInfo* pipelineInfo = copySurface_getOrCreateGraphicsPipeline(state);
	VKRObjectFramebuffer* framebuffer = surfaceCopy_getOrCreateFramebuffer(state, pipelineInfo);
	framebuffer->flagForCurrentCommandBuffer();
	VKRObjectDescriptorSet* descriptorSet = surfaceCopy_getOrCreateDescriptorSet(state, pipelineInfo);
	VKRObjectPipeline* pipelineObj = pipelineInfo->vkObjPipeline;
	VkCommandBuffer cmd = m_state.currentCommandBuffer;

	// full target quad, source texel offset zero
	const CopyPushConstants push = {
		{ -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f },
		{ 0, 0 },
	};
	vkCmdPushConstants(cmd, pipelineObj->m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

	// flipped viewport, the copy covers the top left effectiveCopyWidth x effectiveCopyHeight
	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = (float)effectiveCopyHeight;
	viewport.width = (float)effectiveCopyWidth;
	viewport.height = (float)-effectiveCopyHeight;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	VkRect2D area;
	area.offset = { 0, 0 };
	area.extent = { (uint32)effectiveCopyWidth, (uint32)effectiveCopyHeight };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &area);

	cemu_assert_debug(srcTextureVk->GetImageObj()->m_image != dstTextureVk->GetImageObj()->m_image);
	// earlier writes to the source and any earlier access to the destination complete first
	barrier_image<SYNC_OP::IMAGE_WRITE | SYNC_OP::ANY_TRANSFER, SYNC_OP::IMAGE_READ>(srcTextureVk, srcLayers, srcTextureVk->GetDefaultLayout());
	barrier_image<SYNC_OP::IMAGE_READ | SYNC_OP::IMAGE_WRITE | SYNC_OP::ANY_TRANSFER, SYNC_OP::IMAGE_WRITE>(dstTextureVk, dstLayers, dstTextureVk->GetDefaultLayout());

	VkRenderPassBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = pipelineInfo->vkObjRenderPass->m_renderPass;
	begin.framebuffer = framebuffer->m_frameBuffer;
	begin.renderArea = area;
	begin.clearValueCount = 0;
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineObj->GetPipeline());
	pipelineObj->flagForCurrentCommandBuffer();
	m_state.currentPipeline = pipelineObj->GetPipeline();
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineObj->m_pipelineLayout, 0, 1, &descriptorSet->descriptorSet, 0, nullptr);
	descriptorSet->flagForCurrentCommandBuffer();
	vkCmdDraw(cmd, 6, 1, 0, 0);
	vkCmdEndRenderPass(cmd);

	// later access to either image waits for the draw
	barrier_image<SYNC_OP::IMAGE_READ, SYNC_OP::IMAGE_READ | SYNC_OP::IMAGE_WRITE | SYNC_OP::ANY_TRANSFER>(srcTextureVk, srcLayers, srcTextureVk->GetDefaultLayout());
	barrier_image<SYNC_OP::IMAGE_WRITE, SYNC_OP::IMAGE_READ | SYNC_OP::IMAGE_WRITE | SYNC_OP::ANY_TRANSFER>(dstTextureVk, dstLayers, dstTextureVk->GetDefaultLayout());

	// the guest's viewport and scissor are dynamic state too, put them back
	vkCmdSetViewport(cmd, 0, 1, &m_state.currentViewport);
	vkCmdSetScissor(cmd, 0, 1, &m_state.currentScissorRect);

	LatteTexture_TrackTextureGPUWrite(dstTextureVk, texDstSlice, texDstMip, LatteTexture_getNextUpdateEventCounter());
}

void VulkanRenderer::surfaceCopy_copySurfaceWithFormatConversion(LatteTexture* sourceTexture, sint32 srcMip, sint32 srcSlice, LatteTexture* destinationTexture, sint32 dstMip, sint32 dstSlice, sint32 width, sint32 height)
{
	sint32 copyWidth = width;
	sint32 copyHeight = height;
	LatteTexture_scaleToEffectiveSize(sourceTexture, &copyWidth, &copyHeight, 0);

	auto* src = (LatteTextureVk*)sourceTexture;
	auto* dst = (LatteTextureVk*)destinationTexture;
	// a drawcall copy cannot rescale and does not convert between texel sizes
	if (!LatteTexture_doesEffectiveRescaleRatioMatch(src, srcMip, dst, dstMip))
	{
		cemuLog_logDebug(LogType::Force, "surfaceCopy_copySurfaceViaDrawcall(): Mismatching dimensions");
		return;
	}
	if (src->GetBPP() != dst->GetBPP())
	{
		cemuLog_logDebug(LogType::Force, "surfaceCopy_copySurfaceViaDrawcall(): Mismatching BPP");
		return;
	}
	surfaceCopy_viaDrawcall(src, srcMip, srcSlice, dst, dstMip, dstSlice, copyWidth, copyHeight);
}

// Called when a texture is destroyed; nothing uses its views, framebuffers or descriptor sets anymore.
void VulkanRenderer::surfaceCopy_notifyTextureRelease(LatteTextureVk* hostTexture)
{
	for (auto& [key, pipelineInfo] : m_copySurfacePipelineCache)
	{
		auto descriptors = pipelineInfo->map_descriptors.find(hostTexture);
		if (descriptors != pipelineInfo->map_descriptors.end())
		{
			descriptors->second.ForEach([](CopySurfacePipelineInfo::DescriptorValue& v) {
				Release(v.vkObjDescriptorSet);
				Release(v.vkObjImageView);
			});
			pipelineInfo->map_descriptors.erase(descriptors);
		}
		auto framebuffers = pipelineInfo->map_framebuffers.find(hostTexture);
		if (framebuffers != pipelineInfo->map_framebuffers.end())
		{
			framebuffers->second.ForEach([](CopySurfacePipelineInfo::FramebufferValue& v) { CopySurfacePipelineInfo::ReleaseFramebuffer(v); });
			pipelineInfo->map_framebuffers.erase(framebuffers);
		}
	}
}

void VulkanRenderer::surfaceCopy_cleanup()
{
	for (auto& [key, pipelineInfo] : m_copySurfacePipelineCache)
		delete pipelineInfo;
	m_copySurfacePipelineCache = {};
}
