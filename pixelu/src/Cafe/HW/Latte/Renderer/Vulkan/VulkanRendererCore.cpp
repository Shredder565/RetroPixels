// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/RendererShaderVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanPipelineCompiler.h"

#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteIndices.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "imgui/imgui_impl_vulkan.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "util/helpers/helpers.h"

extern bool hasValidFramebufferAttached;

// Defined in the Common renderer
void LatteDraw_handleSpecialState8_clearAsDepth();

namespace
{
	// order dependent 64-bit mix used for the in-memory pipeline and descriptor set keys
	class StateKey
	{
	public:
		StateKey& Add(uint64 v)
		{
			m_value ^= v + 0x9E3779B97F4A7C15ull + (m_value << 6) + (m_value >> 2);
			m_value *= 0xBF58476D1CE4E5B9ull;
			m_value ^= m_value >> 31;
			return *this;
		}
		uint64 Get() const { return m_value; }

	private:
		uint64 m_value = 0x6A09E667F3BCC908ull;
	};

	// where a shader stage finds its textures and samplers
	struct StageTextureRegs
	{
		uint32 hostUnitBase; // into VulkanRenderer::m_state.boundTexture
		uint32 resourceRegBase; // SQ_TEX_RESOURCE_WORD0 of unit 0
		uint32 samplerBase; // index of the stage's first sampler in SQ_TEX_SAMPLER
	};

	StageTextureRegs TextureRegsOf(LatteConst::ShaderType type)
	{
		switch (type)
		{
		case LatteConst::ShaderType::Vertex:
			return { LATTE_CEMU_VS_TEX_UNIT_BASE, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS, (uint32)Latte::SAMPLER_BASE_INDEX_VERTEX };
		case LatteConst::ShaderType::Pixel:
			return { LATTE_CEMU_PS_TEX_UNIT_BASE, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, (uint32)Latte::SAMPLER_BASE_INDEX_PIXEL };
		case LatteConst::ShaderType::Geometry:
			return { LATTE_CEMU_GS_TEX_UNIT_BASE, Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS, (uint32)Latte::SAMPLER_BASE_INDEX_GEOMETRY };
		default:
			UNREACHABLE;
		}
	}

	// offset of a stage's ALU constants within SQ_ALU_CONSTANT (the geometry stage has none)
	uint32 AluConstOffsetOf(LatteConst::ShaderType type)
	{
		return type == LatteConst::ShaderType::Vertex ? 0x400 : 0;
	}

	constexpr uint32 kSwizzleBits = 0x0FFF0000; // component selects in SQ_TEX_RESOURCE_WORD4

	// CPU side staging of the uniform data of each stage
	float s_vsUniformStaging[512 * 4];
	float s_psUniformStaging[512 * 4];
	float s_gsUniformStaging[512 * 4];
}

// Only state that can change between draws of one sequence without ending it; draws of a sequence
// compare this against the pipeline in use.
uint64 VulkanRenderer::draw_calculateMinimalGraphicsPipelineHash(const LatteFetchShader* fetchShader, const LatteContextRegister& lcr)
{
	StateKey key;
	for (const auto& group : fetchShader->bufferGroups)
		key.Add(group.getCurrentBufferStride(lcr.GetRawView()));
	key.Add(fetchShader->getVkPipelineHashFragment());
	key.Add(lcr.GetRawView()[mmVGT_PRIMITIVE_TYPE]);
	key.Add(lcr.GetRawView()[mmVGT_STRMOUT_EN]);
	key.Add(lcr.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() ? 1 : 0);
	return key.Get();
}

// Everything a graphics pipeline is built from. The fetch shader passed in is the active one: the
// vertex shader's compatibleFetchShader has the same layout but possibly other offsets, and offsets
// are baked into the pipeline.
uint64 VulkanRenderer::draw_calculateGraphicsPipelineHash(const LatteFetchShader* fetchShader, const LatteDecompilerShader* vertexShader, const LatteDecompilerShader* geometryShader, const LatteDecompilerShader* pixelShader, const VKRObjectRenderPass* renderPassObj, const LatteContextRegister& lcr)
{
	cemu_assert_debug(vertexShader->compatibleFetchShader->key == fetchShader->key);
	const uint32* regs = lcr.GetRawView();

	StateKey key;
	key.Add(draw_calculateMinimalGraphicsPipelineHash(fetchShader, lcr));
	key.Add(vertexShader ? vertexShader->baseHash : 0);
	key.Add(geometryShader ? geometryShader->baseHash : 0);
	key.Add(pixelShader ? pixelShader->baseHash + pixelShader->auxHash : 0);

	const uint32 polygonControl = lcr.PA_SU_SC_MODE_CNTL.getRawValue();
	key.Add(polygonControl);
	key.Add(regs[Latte::REGADDR::PA_CL_CLIP_CNTL]);

	// blend: colour control, write mask and the controls of the render targets that blend
	const uint32 colorControl = regs[Latte::REGADDR::CB_COLOR_CONTROL];
	key.Add(colorControl);
	key.Add(regs[Latte::REGADDR::CB_TARGET_MASK]);
	const uint32 blendMask = (colorControl >> 8) & 0xFF;
	for (uint32 rt = 0; rt < 8; rt++)
	{
		if (blendMask & (1u << rt))
			key.Add(((uint64)rt << 32) | regs[Latte::REGADDR::CB_BLEND0_CONTROL + rt]);
	}

	key.Add(renderPassObj->m_hashForPipeline);

	// depth/stencil: the stencil part of DB_DEPTH_CONTROL and the stencil masks only count with stencil on
	uint32 depthControl = regs[Latte::REGADDR::DB_DEPTH_CONTROL];
	const bool stencilOn = (depthControl & 1) != 0;
	if (stencilOn)
	{
		key.Add(regs[mmDB_STENCILREFMASK]);
		const bool backStencilOn = (depthControl & (1 << 7)) != 0;
		key.Add(backStencilOn ? regs[mmDB_STENCILREFMASK_BF] : 0);
	}
	else
	{
		depthControl &= 0xFF;
	}
	key.Add(depthControl);
	return key.Get();
}

void VulkanRenderer::draw_debugPipelineHashState()
{
	cemu_assert_debug(false);
}

PipelineInfo* VulkanRenderer::draw_getCachedPipeline()
{
	const LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader();
	auto perVertexShader = m_pipeline_info_cache.find(vs->baseHash);
	if (perVertexShader == m_pipeline_info_cache.end())
		return nullptr;
	const uint64 stateHash = draw_calculateGraphicsPipelineHash(LatteSHRC_GetActiveFetchShader(), vs, LatteSHRC_GetActiveGeometryShader(), LatteSHRC_GetActivePixelShader(),
		m_state.activeFBO->GetRenderPassObj(), LatteGPUState.contextNew);
	auto entry = perVertexShader->second.find(stateHash);
	return entry != perVertexShader->second.end() ? entry->second : nullptr;
}

void VulkanRenderer::unregisterGraphicsPipeline(PipelineInfo* pipelineInfo)
{
	// a pipeline is registered once, remove the first match
	for (auto& [vsHash, pipelines] : m_pipeline_info_cache)
	{
		for (auto it = pipelines.begin(); it != pipelines.end(); ++it)
		{
			if (it->second == pipelineInfo)
			{
				pipelines.erase(it);
				return;
			}
		}
	}
}

// Whether a draw may be skipped while its pipeline compiles in the background. Draws that likely
// produce lasting results (render to texture passes) are not skipped.
bool VulkanRenderer::IsAsyncPipelineAllowed(uint32 numIndices)
{
	if (IsTracingToolEnabled())
		return false; // frame debuggers do not cope with skipped draws
	const VkExtent2D& extent = m_state.activeFBO->GetExtend();
	if (extent.width == 1600 && extent.height == 1600)
		return false; // Splatoon's ink coverage maps; rare enough to exclude by size
	if (m_state.activeFBO->hasDepthBuffer())
		return true;
	// a handful of indices is usually a full screen quad, often building a persistent texture
	return numIndices > 6;
}

PipelineInfo* VulkanRenderer::draw_createGraphicsPipeline(uint32 indexCount)
{
	LatteFetchShader* fs = LatteSHRC_GetActiveFetchShader();
	LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader();
	LatteDecompilerShader* gs = LatteSHRC_GetActiveGeometryShader();
	LatteDecompilerShader* ps = LatteSHRC_GetActivePixelShader();
	CachedFBOVk* fbo = m_state.activeFBO;

	const uint64 minimalHash = draw_calculateMinimalGraphicsPipelineHash(fs, LatteGPUState.contextNew);
	const uint64 fullHash = draw_calculateGraphicsPipelineHash(fs, vs, gs, ps, fbo->GetRenderPassObj(), LatteGPUState.contextNew);

	auto* pipelineInfo = new PipelineInfo(minimalHash, fullHash, fs, vs, ps, gs);
	m_pipeline_info_cache[vs->baseHash].emplace(fullHash, pipelineInfo);

	auto* compiler = new PipelineCompiler();
	compiler->InitFromCurrentGPUState(pipelineInfo, LatteGPUState.contextNew, fbo->GetRenderPassObj(), PipelineCompiler::CalcRobustBufferAccessRequirement(vs, ps, gs));
	compiler->TrackAsCached(vs->baseHash, fullHash);

	const bool mayCompileAsync = GetConfig().async_compile && IsAsyncPipelineAllowed(indexCount);
	// try without blocking first, which only succeeds when the driver has the pipeline cached;
	// otherwise compile in the background (async allowed) or right now
	if (mayCompileAsync && !compiler->Compile(false, true, true))
	{
		PipelineCompiler::CompileThreadPool_QueueCompilation(compiler);
		return pipelineInfo;
	}
	if (!mayCompileAsync)
		compiler->Compile(true, true, true);
	delete compiler;
	return pipelineInfo;
}

PipelineInfo* VulkanRenderer::draw_getOrCreateGraphicsPipeline(uint32 indexCount)
{
	PipelineInfo* cached = draw_getCachedPipeline();
	if (!cached)
		return draw_createGraphicsPipeline(indexCount);
#ifdef CEMU_DEBUG_ASSERT
	cemu_assert_debug(cached->vertexShader == LatteSHRC_GetActiveVertexShader());
	cemu_assert_debug(cached->geometryShader == LatteSHRC_GetActiveGeometryShader());
	cemu_assert_debug(cached->pixelShader == LatteSHRC_GetActivePixelShader());
	const LatteFetchShader* activeFetch = LatteSHRC_GetActiveFetchShader();
	if (cached->fetchShader->key != activeFetch->key || cached->fetchShader->vkPipelineHashFragment != activeFetch->vkPipelineHashFragment)
	{
		debug_printf("Incompatible fetch shader %p %p\n", cached->fetchShader, activeFetch);
		assert_dbg();
	}
	cemu_assert_debug(cached->primitiveMode == LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE());
	cemu_assert_debug(cached->minimalStateHash == draw_calculateMinimalGraphicsPipelineHash(activeFetch, LatteGPUState.contextNew));
#endif
	return cached;
}

Renderer::IndexAllocation VulkanRenderer::indexData_reserveIndexMemory(uint32 size)
{
	auto* reservation = memoryManager->GetIndexAllocator().AllocateBufferMemory(size, 32);
	return { reservation->memPtr, reservation };
}

void VulkanRenderer::indexData_releaseIndexMemory(IndexAllocation& allocation)
{
	memoryManager->GetIndexAllocator().FreeReservation((VKRSynchronizedHeapAllocator::AllocatorReservation*)allocation.rendererInternal);
}

void VulkanRenderer::indexData_uploadIndexMemory(IndexAllocation& allocation)
{
	memoryManager->GetIndexAllocator().FlushReservation((VKRSynchronizedHeapAllocator::AllocatorReservation*)allocation.rendererInternal);
}

// Appends uniform data to the ring buffer and returns its offset (used as dynamic offset). Space
// is freed as command buffers finish; when the ring is full we wait for the next one.
uint32 VulkanRenderer::uniformData_uploadUniformDataBufferGetOffset(std::span<uint8> data)
{
	const uint32 alignMask = m_featureControl.limits.calcUniformBufferAlignmentM1;
	const uint32 allocSize = ((uint32)data.size() + alignMask) & ~alignMask;

	auto waitUntil = [this](auto&& done) {
		while (!done())
		{
			if (m_commandBufferSyncIndex == m_commandBufferIndex)
			{
				// everything submitted has finished and it still does not fit; overwrite instead of
				// stalling forever (submitting here would lose the dynamic state)
				cemuLog_log(LogType::Force, "Vulkan: Uniform ring buffer overflowed. Expect visual corruption");
				cemu_assert_suspicious();
				return;
			}
			WaitForNextFinishedCommandBuffer();
		}
	};

	// data is never split, wrap to the start when the tail is too short
	if (m_uniformVarBufferWriteIndex + allocSize > UNIFORMVAR_RINGBUFFER_SIZE)
	{
		// the reader has to be ahead of the start and behind the writer first
		waitUntil([this]() { return m_uniformVarBufferReadIndex <= m_uniformVarBufferWriteIndex && m_uniformVarBufferReadIndex != 0; });
		m_uniformVarBufferWriteIndex = 0;
	}
	auto freeBytes = [this]() -> sint64 {
		sint64 used = (sint64)m_uniformVarBufferWriteIndex - (sint64)m_uniformVarBufferReadIndex;
		if (used < 0)
			used += UNIFORMVAR_RINGBUFFER_SIZE;
		return (sint64)UNIFORMVAR_RINGBUFFER_SIZE - 1 - used;
	};
	waitUntil([&]() { return freeBytes() >= (sint64)allocSize; });

	const uint32 offset = m_uniformVarBufferWriteIndex;
	memcpy(m_uniformVarBufferPtr + offset, data.data(), data.size());
	m_uniformVarBufferWriteIndex += allocSize;
	if (!m_uniformVarBufferMemoryIsCoherent)
	{
		VkMappedMemoryRange range{};
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = m_uniformVarBufferMemory;
		range.offset = offset;
		range.size = allocSize;
		vkFlushMappedMemoryRanges(m_logicalDevice, 1, &range);
	}
	return offset;
}

// Fills the stage's uniform block (emulator provided values plus guest constants) and uploads it.
void VulkanRenderer::uniformData_updateUniformVars(uint32 shaderStageIndex, LatteDecompilerShader* shader, float* __restrict uniformBuf)
{
	if (shader->resourceMapping.uniformVarsBufferBindingPoint < 0)
		return;
	const auto& uf = shader->uniform;
	auto at = [uniformBuf](size_t byteOffset) { return uniformBuf + byteOffset / 4; };

	for (auto& rescale : shader->uniform.list_ufTexRescale)
	{
		const float* scale = LatteTexture_getEffectiveTextureScale(shader->shaderType, rescale.texUnit);
		memcpy(rescale.currentValue, scale, sizeof(float) * 2);
		memcpy(at(rescale.uniformLocation), scale, sizeof(float) * 2);
	}
	if (uf.loc_alphaTestRef >= 0)
		*at(uf.loc_alphaTestRef) = LatteGPUState.contextNew.SX_ALPHA_REF.get_ALPHA_TEST_REF();
	if (uf.loc_pointSize >= 0)
	{
		// 12.4 fixed point, zero becomes the smallest size
		float width = (float)LatteGPUState.contextNew.PA_SU_POINT_SIZE.get_WIDTH() / 8.0f;
		*at(uf.loc_pointSize) = width == 0.0f ? 1.0f / 8.0f : width;
	}
	if (uf.loc_remapped >= 0)
		LatteBufferCache_LoadRemappedUniforms(shader, at(uf.loc_remapped), true, (1 << LATTE_NUM_MAX_UNIFORM_BUFFERS) - 1);
	if (uf.loc_uniformRegister >= 0)
	{
		const uint32* aluConsts = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + AluConstOffsetOf(shader->shaderType);
		memcpy(at(uf.loc_uniformRegister), aluConsts, uf.count_uniformRegister * 16);
	}
	if (uf.loc_windowSpaceToClipSpaceTransform >= 0)
	{
		sint32 vpWidth, vpHeight;
		LatteRenderTarget_GetCurrentVirtualViewportSize(&vpWidth, &vpHeight); // the viewport is up to date at this point
		float* transform = at(uf.loc_windowSpaceToClipSpaceTransform);
		transform[0] = 2.0f / (float)vpWidth;
		transform[1] = 2.0f / (float)vpHeight;
	}
	if (uf.loc_fragCoordScale >= 0)
		LatteMRT::GetCurrentFragCoordScale(at(uf.loc_fragCoordScale));
	if (uf.loc_verticesPerInstance >= 0)
	{
		*(int*)at(uf.loc_verticesPerInstance) = m_streamoutState.verticesPerInstance;
		for (sint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
		{
			if (uf.loc_streamoutBufferBase[b] >= 0)
				*(uint32*)at(uf.loc_streamoutBufferBase[b]) = m_streamoutState.buffer[b].ringBufferOffset;
		}
	}
	dynamicOffsetInfo.uniformVarBufferOffset[shaderStageIndex] = uniformData_uploadUniformDataBufferGetOffset({ (uint8*)uniformBuf, uf.uniformRangeSize });
}

// Follow-up draws of a sequence: only guest constants can have changed (anything else ends the
// sequence), and the block is uploaded only if they did.
void VulkanRenderer::uniformData_updateUniformVarsIncremental(uint32 shaderStageIndex, LatteDecompilerShader* shader, uint8& stageUniformModifiedMask, float* __restrict uniformBuf, bool aluConstDirty, uint32 uniformBufferDirtyMask)
{
	if (shader->resourceMapping.uniformVarsBufferBindingPoint < 0)
		return;
	const auto& uf = shader->uniform;
	auto at = [uniformBuf](size_t byteOffset) { return uniformBuf + byteOffset / 4; };

	bool changed = false;
	if (uf.loc_remapped >= 0)
	{
		cemu_assert_debug(shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_REMAPPED);
		changed = LatteBufferCache_LoadRemappedUniforms(shader, at(uf.loc_remapped), aluConstDirty, uniformBufferDirtyMask);
	}
	if (uf.loc_uniformRegister >= 0 && aluConstDirty)
	{
		cemu_assert_debug(shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CFILE);
		if (shader->shaderType == LatteConst::ShaderType::Geometry)
			cemu_assert_suspicious(); // no ALU constants in the geometry stage
		const uint32* aluConsts = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + AluConstOffsetOf(shader->shaderType);
		memcpy(at(uf.loc_uniformRegister), aluConsts, uf.count_uniformRegister * 16);
		changed = true;
	}
	if (!changed)
		return;
	dynamicOffsetInfo.uniformVarBufferOffset[shaderStageIndex] = uniformData_uploadUniformDataBufferGetOffset({ (uint8*)uniformBuf, uf.uniformRangeSize });
	stageUniformModifiedMask |= (1 << shaderStageIndex);
}

// dynamic offsets in binding order: the uniform block first, then each guest uniform buffer
void VulkanRenderer::draw_prepareDynamicOffsetsForDescriptorSet(uint32 shaderStageIndex, uint32* dynamicOffsets, sint32& numDynOffsets, const PipelineInfo* pipeline_info)
{
	const auto& layout = pipeline_info->dynamicOffsetInfo;
	numDynOffsets = 0;
	if (layout.hasUniformVar[shaderStageIndex])
		dynamicOffsets[numDynOffsets++] = dynamicOffsetInfo.uniformVarBufferOffset[shaderStageIndex];
	if (layout.hasUniformBuffers[shaderStageIndex])
	{
		for (uint8 ub : layout.list_uniformBuffers[shaderStageIndex])
			dynamicOffsets[numDynOffsets++] = dynamicOffsetInfo.shaderUB[shaderStageIndex].uniformBufferOffset[ub];
	}
}

// Key of the descriptor set a draw needs: per texture binding the bound view, its swizzle and the
// sampler registers.
uint64 VulkanRenderer::GetDescriptorSetStateHash(LatteDecompilerShader* shader)
{
	const StageTextureRegs stage = TextureRegsOf(shader->shaderType);
	const uint32* regs = LatteGPUState.contextRegister;
	const sint32 textureCount = shader->resourceMapping.getTextureCount();

	StateKey key;
	for (sint32 binding = 0; binding < textureCount; binding++)
	{
		const auto unit = shader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(binding);
		LatteTextureViewVk* view = m_state.boundTexture[stage.hostUnitBase + unit];
		if (!view)
			continue;
		key.Add(binding);
		const uint32 stageSampler = shader->textureUnitSamplerAssignment[unit];
		if (stageSampler != LATTE_DECOMPILER_SAMPLER_NONE) [[likely]]
		{
			const uint32* samplerRegs = regs + Latte::REGADDR::SQ_TEX_SAMPLER_WORD0_0 + (stage.samplerBase + stageSampler) * 3;
			key.Add(samplerRegs[0]).Add(samplerRegs[1]).Add(samplerRegs[2]);
		}
		key.Add(view->GetUniqueId());
		key.Add(regs[stage.resourceRegBase + unit * 7 + 4] & kSwizzleBits);
	}
	return key.Get();
}

namespace
{
	constexpr VkSamplerAddressMode s_addressModes[8] = {
		VK_SAMPLER_ADDRESS_MODE_REPEAT, // WRAP
		VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, // MIRROR
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // CLAMP_LAST_TEXEL
		VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, // MIRROR_ONCE_LAST_TEXEL
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // CLAMP_HALF_BORDER (approximated)
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, // MIRROR_ONCE_HALF_BORDER (approximated)
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, // CLAMP_BORDER
		VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, // MIRROR_ONCE_BORDER (approximated)
	};

	constexpr VkCompareOp s_samplerCompareOps[8] = {
		VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
		VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS,
	};

	VkFilter XYFilterOf(Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER f)
	{
		using XY = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER;
		return (f == XY::POINT || f == XY::ANISO_POINT) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	}

	// Translates a guest sampler into Vulkan sampler state; the custom border colour struct is
	// chained when used, so it has to outlive the create info.
	void TranslateSampler(const _LatteRegisterSetSampler& sampler, const _LatteRegisterSetSamplerBorderColor& borderColor, LatteTextureVk* texture, bool depthCompare,
		bool hasCustomBorderColor, VkSamplerCreateInfo& info, VkSamplerCustomBorderColorCreateInfoEXT& customBorder)
	{
		using W0 = Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0;
		const auto& overrides = texture->overwriteInfo;

		// LOD values are 4.6 fixed point; graphic packs can shift or replace the bias
		const float minLod = (float)sampler.WORD1.get_MIN_LOD() / 64.0f;
		const float maxLod = (float)sampler.WORD1.get_MAX_LOD() / 64.0f;
		sint32 lodBias = sampler.WORD1.get_LOD_BIAS();
		if (overrides.hasRelativeLodBias)
			lodBias += overrides.relativeLodBias;
		if (overrides.hasLodBias)
			lodBias = overrides.lodBias;
		info.mipLodBias = (float)lodBias / 64.0f;

		switch (sampler.WORD0.get_MIP_FILTER())
		{
		case W0::E_Z_FILTER::NONE:
			// no mipmapping: stay on the base level but keep the min/mag decision
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			info.minLod = 0.0f;
			info.maxLod = 0.25f;
			break;
		case W0::E_Z_FILTER::POINT:
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			info.minLod = minLod;
			info.maxLod = maxLod;
			break;
		default: // LINEAR, and invalid values
			info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			info.minLod = minLod;
			info.maxLod = maxLod;
			break;
		}

		cemu_assert_debug(sampler.WORD0.get_XY_MIN_FILTER() != W0::E_XY_FILTER::BICUBIC);
		info.minFilter = XYFilterOf(sampler.WORD0.get_XY_MIN_FILTER());
		info.magFilter = XYFilterOf(sampler.WORD0.get_XY_MAG_FILTER());
		// the Z filter of array textures has no Vulkan equivalent

		info.addressModeU = s_addressModes[(size_t)sampler.WORD0.get_CLAMP_X()];
		info.addressModeV = s_addressModes[(size_t)sampler.WORD0.get_CLAMP_Y()];
		info.addressModeW = s_addressModes[(size_t)sampler.WORD0.get_CLAMP_Z()];

		auto anisoLog2 = sampler.WORD0.get_MAX_ANISO_RATIO();
		if (overrides.anisotropicLevel >= 0)
			anisoLog2 = overrides.anisotropicLevel;
		info.anisotropyEnable = anisoLog2 > 0 ? VK_TRUE : VK_FALSE;
		info.maxAnisotropy = anisoLog2 > 0 ? (float)(1 << anisoLog2) : 1.0f;

		info.compareEnable = depthCompare ? VK_TRUE : VK_FALSE;
		if (depthCompare)
			info.compareOp = s_samplerCompareOps[(size_t)sampler.WORD0.get_DEPTH_COMPARE_FUNCTION()];

		switch (sampler.WORD0.get_BORDER_COLOR_TYPE())
		{
		case W0::E_BORDER_COLOR_TYPE::TRANSPARENT_BLACK:
			info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			break;
		case W0::E_BORDER_COLOR_TYPE::OPAQUE_BLACK:
			info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
			break;
		case W0::E_BORDER_COLOR_TYPE::OPAQUE_WHITE:
			info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
			break;
		default:
			// register supplied colour, transparent black where the extension is missing
			if (!hasCustomBorderColor)
			{
				info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
				break;
			}
			customBorder.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
			customBorder.format = VK_FORMAT_UNDEFINED;
			customBorder.customBorderColor.float32[0] = borderColor.red.get_channelValue();
			customBorder.customBorderColor.float32[1] = borderColor.green.get_channelValue();
			customBorder.customBorderColor.float32[2] = borderColor.blue.get_channelValue();
			customBorder.customBorderColor.float32[3] = borderColor.alpha.get_channelValue();
			info.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
			info.pNext = &customBorder;
			break;
		}
	}

	const _LatteRegisterSetSamplerBorderColor& BorderColorRegs(LatteConst::ShaderType type, uint32 stageSampler)
	{
		switch (type)
		{
		case LatteConst::ShaderType::Vertex:
			return LatteGPUState.contextNew.TD_VS_SAMPLER_BORDER_COLOR[stageSampler];
		case LatteConst::ShaderType::Pixel:
			return LatteGPUState.contextNew.TD_PS_SAMPLER_BORDER_COLOR[stageSampler];
		default:
			return LatteGPUState.contextNew.TD_GS_SAMPLER_BORDER_COLOR[stageSampler];
		}
	}
}

VkDescriptorSetInfo* VulkanRenderer::draw_getOrCreateDescriptorSet(PipelineInfo* pipeline_info, LatteDecompilerShader* shader)
{
	const uint64 stateHash = GetDescriptorSetStateHash(shader);
	cemu_assert_debug(shader->shaderType == LatteConst::ShaderType::Vertex || shader->shaderType == LatteConst::ShaderType::Pixel || shader->shaderType == LatteConst::ShaderType::Geometry);
	auto& cache = pipeline_info->GetDescriptorSetCache(shader->shaderType);
	if (auto it = cache.find(stateHash); it != cache.end())
		return it->second;

	VKRObjectPipeline* pipelineObj = pipeline_info->m_vkrObjPipeline;
	VkDescriptorSetLayout setLayout;
	switch (shader->shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		setLayout = pipelineObj->m_vertexDSL;
		break;
	case LatteConst::ShaderType::Pixel:
		setLayout = pipelineObj->m_pixelDSL;
		break;
	case LatteConst::ShaderType::Geometry:
		setLayout = pipelineObj->m_geometryDSL;
		break;
	default:
		UNREACHABLE;
	}

	auto* dsInfo = new VkDescriptorSetInfo();
	dsInfo->stateHash = stateHash;
	dsInfo->shaderType = shader->shaderType;
	dsInfo->pipeline_info = pipeline_info;
	dsInfo->m_vkObjDescriptorSet = new VKRObjectDescriptorSet();
	VKRObjectDescriptorSet* setObj = dsInfo->m_vkObjDescriptorSet;

	VkDescriptorSetAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorPool = m_descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &setLayout;
	VkDescriptorSet set;
	if (vkAllocateDescriptorSets(m_logicalDevice, &allocInfo, &set) != VK_SUCCESS)
	{
		UnrecoverableError(fmt::format("Failed to allocate descriptor sets. Currently allocated: Descriptors={} TextureSamplers={} DynUniformBuffers={} StorageBuffers={}",
			performanceMonitor.vk.numDescriptorSets.get(),
			performanceMonitor.vk.numDescriptorSamplerTextures.get(),
			performanceMonitor.vk.numDescriptorDynUniformBuffers.get(),
			performanceMonitor.vk.numDescriptorStorageBuffers.get()).c_str());
	}
	setObj->descriptorSet = set;

	// textures
	const StageTextureRegs stage = TextureRegsOf(shader->shaderType);
	const sint32 textureCount = shader->resourceMapping.getTextureCount();
	std::vector<VkDescriptorImageInfo> images;
	images.reserve(textureCount);
	for (sint32 binding = 0; binding < textureCount; binding++)
	{
		const auto unit = shader->resourceMapping.getRelativeTextureUnitFromRelativeBindingPoint(binding);
		const Latte::E_DIM shaderDim = shader->textureUnitDim[unit];
		LatteTextureViewVk* view = m_state.boundTexture[stage.hostUnitBase + unit];

		// nothing bound, or a view the shader cannot sample (1D vs 2D mismatch): a null texture
		bool useNull = !view;
		if (view && shaderDim == Latte::E_DIM::DIM_1D && view->dim != Latte::E_DIM::DIM_1D)
			useNull = true;
		if (view && shaderDim == Latte::E_DIM::DIM_2D && view->dim != Latte::E_DIM::DIM_2D && view->dim != Latte::E_DIM::DIM_2D_MSAA)
			useNull = true;
		if (useNull)
		{
			const auto& nullTex = shaderDim == Latte::E_DIM::DIM_1D ? nullTexture1D : nullTexture2D;
			VkDescriptorImageInfo& img = images.emplace_back();
			img.sampler = nullTex.sampler;
			img.imageView = nullTex.view;
			img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			continue;
		}

		LatteTextureVk* texture = static_cast<LatteTextureVk*>(view->baseTexture);
		const uint32 word4 = LatteGPUState.contextRegister[stage.resourceRegBase + unit * 7 + 4];
		VKRObjectTextureView* imageView = view->GetSamplerView(word4);
		setObj->addRef(imageView);
		// the set has to go when the view goes
		vectorAppendUnique(dsInfo->list_referencedViews, view);
		view->AddDescriptorSetReference(dsInfo);
		// sampled textures that might also be render targets, for barrier decisions
		if (!texture->IsCompressedFormat())
			vectorAppendUnique(dsInfo->list_fboCandidates, texture);

		VkSamplerCreateInfo samplerInfo{};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		VkSamplerCustomBorderColorCreateInfoEXT customBorder{};
		const uint32 stageSampler = shader->textureUnitSamplerAssignment[unit];
		if (stageSampler != LATTE_DECOMPILER_SAMPLER_NONE)
		{
			const _LatteRegisterSetSampler& sampler = LatteGPUState.contextNew.SQ_TEX_SAMPLER[stage.samplerBase + stageSampler];
			TranslateSampler(sampler, BorderColorRegs(shader->shaderType, stageSampler), texture, shader->textureUsesDepthCompare[unit],
				m_featureControl.deviceExtensions.custom_border_color_without_format, samplerInfo, customBorder);
		}
		VKRObjectSampler* samplerObj = VKRObjectSampler::GetOrCreateSampler(&samplerInfo);
		setObj->addRef(samplerObj);

		VkDescriptorImageInfo& img = images.emplace_back();
		img.sampler = samplerObj->GetSampler();
		img.imageView = imageView->m_textureImageView;
		img.imageLayout = texture->GetDefaultLayout();
	}

	std::vector<VkWriteDescriptorSet> writes;
	auto addWrite = [&](uint32 binding, VkDescriptorType type) -> VkWriteDescriptorSet& {
		VkWriteDescriptorSet& w = writes.emplace_back();
		w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = set;
		w.dstBinding = binding;
		w.dstArrayElement = 0;
		w.descriptorType = type;
		w.descriptorCount = 1;
		return w;
	};

	for (sint32 i = 0; i < textureCount; i++)
	{
		addWrite(shader->resourceMapping.getTextureBaseBindingPoint() + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER).pImageInfo = images.data() + i;
		performanceMonitor.vk.numDescriptorSamplerTextures.increment();
		dsInfo->statsNumSamplerTextures++;
	}

	// emulator uniform block, addressed through the dynamic offset only
	VkDescriptorBufferInfo uniformBlock{};
	if (shader->resourceMapping.uniformVarsBufferBindingPoint >= 0)
	{
		uniformBlock.buffer = m_uniformVarBuffer;
		uniformBlock.offset = 0;
		uniformBlock.range = shader->uniform.uniformRangeSize;
		addWrite(shader->resourceMapping.uniformVarsBufferBindingPoint, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC).pBufferInfo = &uniformBlock;
		performanceMonitor.vk.numDescriptorDynUniformBuffers.increment();
		dsInfo->statsNumDynUniformBuffers++;
	}

	// guest uniform buffers live in the buffer cache (or imported guest memory), also dynamic offsets
	VkDescriptorBufferInfo guestUniforms{};
	guestUniforms.buffer = m_useHostMemoryForCache ? m_importedMem : m_bufferCache;
	guestUniforms.offset = 0;
	// AMD runs with robust buffer access and gets the rest of the buffer; other drivers limit the
	// range of uniform buffers, so they get the largest size games use (64KB, Xenoblade Chronicles X)
	guestUniforms.range = m_vendor == GfxVendor::AMD ? VK_WHOLE_SIZE : 1024 * 16 * 4;
	for (sint32 ub = 0; ub < LATTE_NUM_MAX_UNIFORM_BUFFERS; ub++)
	{
		const sint32 binding = shader->resourceMapping.uniformBuffersBindingPoint[ub];
		if (binding < 0)
			continue;
		addWrite(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC).pBufferInfo = &guestUniforms;
		performanceMonitor.vk.numDescriptorDynUniformBuffers.increment();
		dsInfo->statsNumDynUniformBuffers++;
	}

	// transform feedback ring, the shader computes its own offsets
	VkDescriptorBufferInfo streamout{};
	if (shader->resourceMapping.tfStorageBindingPoint >= 0)
	{
		streamout.buffer = m_xfbRingBuffer;
		streamout.offset = 0;
		streamout.range = VK_WHOLE_SIZE;
		addWrite(shader->resourceMapping.tfStorageBindingPoint, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &streamout;
		performanceMonitor.vk.numDescriptorStorageBuffers.increment();
		dsInfo->statsNumStorageBuffers++;
	}

	if (!writes.empty())
		vkUpdateDescriptorSets(m_logicalDevice, (uint32)writes.size(), writes.data(), 0, nullptr);
	cache[stateHash] = dsInfo;
	return dsInfo;
}

void VulkanRenderer::SurfaceSync(Latte::E_COHER_CNTL coher, MPTR address, uint32 size)
{
	// a guest sync on colour buffers; consumed by the next draw's feedback loop barrier decision
	if (static_cast<uint32>(coher & Latte::E_COHER_CNTL::CB_ALL_DEST_BASE_ENA) != 0)
		m_state.colorBufferSyncPending = true;
}

// Called when the bound textures may have changed. A texture written by a render pass since the
// last barrier (same flush index) and now sampled needs a barrier.
void VulkanRenderer::sync_inputTexturesChanged(bool withinFeedbackLoopRenderPass)
{
	bool needBarrier = withinFeedbackLoopRenderPass; // a feedback loop always needs one
	for (VkDescriptorSetInfo* ds : { m_state.activeVertexDS, m_state.activeGeometryDS, m_state.activePixelDS })
	{
		if (!ds)
			continue;
		for (LatteTextureVk* tex : ds->list_fboCandidates)
		{
			tex->m_vkFlushIndex_read = m_state.currentFlushIndex;
			if (tex->m_vkFlushIndex_write == m_state.currentFlushIndex)
				needBarrier = true;
		}
	}
	if (!needBarrier)
		return;

	// Continuing draws inside a colour-only feedback loop with unchanged descriptors and no guest
	// sync only add feedback hazards; skip the barrier (the read indices above stay updated).
	if (withinFeedbackLoopRenderPass && !m_state.descriptorSetsChanged && !m_state.colorBufferSyncPending
		&& m_state.m_curRenderpassSelfDependencyInfo.GetAspectMask() == VK_IMAGE_ASPECT_COLOR_BIT)
		return;

	constexpr VkPipelineStageFlags attachmentWriteStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	constexpr VkAccessFlags attachmentAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	VkMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = attachmentAccess | VK_ACCESS_SHADER_READ_BIT;
	VkPipelineStageFlags dstStages = attachmentWriteStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	// inside a render pass (feedback loop) only fragment stages and by-region dependencies are allowed
	if (!withinFeedbackLoopRenderPass)
		dstStages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
	const VkDependencyFlags flags = withinFeedbackLoopRenderPass ? VK_DEPENDENCY_BY_REGION_BIT : 0;
	vkCmdPipelineBarrier(m_state.currentCommandBuffer, attachmentWriteStages, dstStages, flags, 1, &barrier, 0, nullptr, 0, nullptr);
	performanceMonitor.vk.numDrawBarriersPerFrame.increment();
	m_state.currentFlushIndex++;
}

// A render pass is about to start on these attachments: they are written from now on. Sampled since
// the last barrier, or written by a previous pass, means a barrier first.
void VulkanRenderer::sync_RenderPassLoadTextures(CachedFBOVk* fboVk)
{
	bool needBarrier = false;
	for (LatteTexture* tex : fboVk->GetTextures())
	{
		auto* texVk = (LatteTextureVk*)tex;
		if (texVk->m_vkFlushIndex_write == m_state.currentFlushIndex || texVk->m_vkFlushIndex_read == m_state.currentFlushIndex)
			needBarrier = true;
		texVk->m_vkFlushIndex_write = m_state.currentFlushIndex;
	}
	if (!needBarrier)
		return;

	VkMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	const VkPipelineStageFlags srcStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	const VkPipelineStageFlags dstStages = srcStages | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	vkCmdPipelineBarrier(m_state.currentCommandBuffer, srcStages, dstStages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
	performanceMonitor.vk.numDrawBarriersPerFrame.increment();
	m_state.currentFlushIndex++;
}

void VulkanRenderer::sync_RenderPassStoreTextures(CachedFBOVk* fboVk)
{
	for (LatteTexture* tex : fboVk->GetTextures())
		((LatteTextureVk*)tex)->m_vkFlushIndex_write = m_state.currentFlushIndex;
}

void VulkanRenderer::draw_prepareDescriptorSets(PipelineInfo* pipeline_info, VkDescriptorSetInfo*& vertexDS, VkDescriptorSetInfo*& pixelDS, VkDescriptorSetInfo*& geometryDS)
{
	auto setFor = [&](LatteDecompilerShader* shader) -> VkDescriptorSetInfo* {
		if (!shader)
			return nullptr;
		VkDescriptorSetInfo* ds = draw_getOrCreateDescriptorSet(pipeline_info, shader);
		ds->m_vkObjDescriptorSet->flagForCurrentCommandBuffer();
		return ds;
	};
	LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader();
	LatteDecompilerShader* gs = LatteSHRC_GetActiveGeometryShader();
	LatteDecompilerShader* ps = LatteSHRC_GetActivePixelShader();
	vertexDS = setFor(vs);
	pixelDS = setFor(ps);
	geometryDS = setFor(gs);
}

void VulkanRenderer::draw_updateVkBlendConstants()
{
	// CB_BLEND_RED..ALPHA hold the constant as floats
	vkCmdSetBlendConstants(m_state.currentCommandBuffer, (const float*)(LatteGPUState.contextRegister + Latte::REGADDR::CB_BLEND_RED));
}

void VulkanRenderer::draw_updateDepthBias(bool forceUpdate)
{
	const auto& ctx = LatteGPUState.contextNew;
	const uint32 scaleBits = ctx.PA_SU_POLY_OFFSET_FRONT_SCALE.getRawValue();
	const uint32 offsetBits = ctx.PA_SU_POLY_OFFSET_FRONT_OFFSET.getRawValue();
	const uint32 clampBits = ctx.PA_SU_POLY_OFFSET_CLAMP.getRawValue();
	const bool unchanged = m_state.prevPolygonFrontScaleU32 == scaleBits && m_state.prevPolygonFrontOffsetU32 == offsetBits && m_state.prevPolygonFrontClampU32 == clampBits;
	if (unchanged && !forceUpdate)
		return;
	m_state.prevPolygonFrontScaleU32 = scaleBits;
	m_state.prevPolygonFrontOffsetU32 = offsetBits;
	m_state.prevPolygonFrontClampU32 = clampBits;
	// the guest slope scale is in 1/16 units
	vkCmdSetDepthBias(m_state.currentCommandBuffer, ctx.PA_SU_POLY_OFFSET_FRONT_OFFSET.get_OFFSET(), ctx.PA_SU_POLY_OFFSET_CLAMP.get_CLAMP(), ctx.PA_SU_POLY_OFFSET_FRONT_SCALE.get_SCALE() / 16.0f);
}

// Begins the render pass of the active FBO unless it is already running and can stay. Sampling a
// render target in the same pass is fine with VK_EXT_attachment_feedback_loop_layout (pixel shader
// only); otherwise the pass is split, which is only done with accurate barriers on.
void VulkanRenderer::draw_setRenderPass()
{
	CachedFBOVk* fbo = m_state.activeFBO;
	auto& selfDep = m_state.m_curRenderpassSelfDependencyInfo;
	if (m_state.descriptorSetsChanged || m_state.activeRenderpassFBO != fbo)
		selfDep = fbo->CheckForSelfDependency(m_state.activeVertexDS, m_state.activeGeometryDS, m_state.activePixelDS);

	const bool loopHandlesIt = UseAttachmentFeedbackLoop() && selfDep.HasSelfDependency() && !selfDep.HasVertexOrGeometrySelfDependency();
	const bool needsSplit = selfDep.HasSelfDependency() && !loopHandlesIt;
	const bool forceNewPass = needsSplit && (GetConfig().vk_accurate_barriers || m_state.activePipelineInfo->neverSkipAccurateBarrier);

	auto updateFeedbackLoop = [&]() {
		if (UseAttachmentFeedbackLoop() && selfDep.GetAspectMask() != m_state.feedbackLoopImageAspect)
		{
			m_state.feedbackLoopImageAspect = selfDep.GetAspectMask();
			vkCmdSetAttachmentFeedbackLoopEnableEXT(m_state.currentCommandBuffer, selfDep.GetAspectMask());
		}
	};

	if (m_state.activeRenderpassFBO == fbo && !forceNewPass)
	{
		if (m_state.descriptorSetsChanged || loopHandlesIt)
			sync_inputTexturesChanged(loopHandlesIt);
		updateFeedbackLoop();
		return;
	}

	draw_endRenderPass();
	if (m_state.descriptorSetsChanged)
		sync_inputTexturesChanged();
	sync_RenderPassLoadTextures(fbo);

	VKRObjectRenderPass* renderPass = fbo->GetRenderPassObj();
	VKRObjectFramebuffer* framebuffer = fbo->GetFramebufferObj();
	if (m_featureControl.deviceExtensions.dynamic_rendering)
	{
		vkCmdBeginRenderingKHR(m_state.currentCommandBuffer, fbo->GetRenderingInfo());
	}
	else
	{
		VkRenderPassBeginInfo begin{};
		begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		begin.renderPass = renderPass->m_renderPass;
		begin.framebuffer = framebuffer->m_frameBuffer;
		begin.renderArea.offset = { 0, 0 };
		begin.renderArea.extent = fbo->GetExtend();
		begin.clearValueCount = 0;
		vkCmdBeginRenderPass(m_state.currentCommandBuffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
	}
	m_state.activeRenderpassFBO = fbo;
	updateFeedbackLoop();
	renderPass->flagForCurrentCommandBuffer();
	framebuffer->flagForCurrentCommandBuffer();
	performanceMonitor.vk.numBeginRenderpassPerFrame.increment();
}

void VulkanRenderer::draw_endRenderPass()
{
	if (!m_state.activeRenderpassFBO)
		return;
	if (m_featureControl.deviceExtensions.dynamic_rendering)
		vkCmdEndRenderingKHR(m_state.currentCommandBuffer);
	else
		vkCmdEndRenderPass(m_state.currentCommandBuffer);
	sync_RenderPassStoreTextures(m_state.activeRenderpassFBO);
	m_state.activeRenderpassFBO = nullptr;
}

// special state 5: copy the depth buffer into colour buffer 0
void VulkanRenderer::draw_handleSpecialState5()
{
	LatteMRT::UpdateCurrentFBO();
	LatteRenderTarget_updateViewport();
	LatteTextureView* color = LatteMRT::GetColorAttachment(0);
	LatteTextureView* depth = LatteMRT::GetDepthAttachment();
	sint32 width, height;
	LatteMRT::GetVirtualViewportDimensions(width, height);
	surfaceCopy_copySurfaceWithFormatConversion(depth->baseTexture, depth->firstMip, depth->firstSlice, color->baseTexture, color->firstMip, color->firstSlice, width, height);
}

// --- draw sequences ---

void VulkanRenderer::draw_beginSequence()
{
	auto& gpu = LatteGPUState;
	m_state.drawSequenceSkip = false;
	const bool streamoutActive = gpu.contextRegister[mmVGT_STRMOUT_EN] != 0;

	LatteSHRC_UpdateActiveShaders();
	if (gpu.activeShaderHasError)
	{
		cemuLog_logDebugOnce(LogType::Force, "Skipping drawcalls due to shader error");
		m_state.drawSequenceSkip = true;
		cemu_assert_debug(false);
		return;
	}

	// texture setup may ask for the render target to be set up again
	gpu.requiresTextureBarrier = false;
	do
	{
		gpu.repeatTextureInitialization = false;
		if (!LatteMRT::UpdateCurrentFBO())
		{
			debug_printf("Rendertarget invalid\n");
			m_state.drawSequenceSkip = true;
			return;
		}
		if (!hasValidFramebufferAttached && !streamoutActive)
		{
			debug_printf("Drawcall with no color buffer or depth buffer attached\n");
			m_state.drawSequenceSkip = true;
			return;
		}
		LatteTexture_updateTextures();
	} while (gpu.repeatTextureInitialization);

	LatteMRT::ApplyCurrentState();
	LatteRenderTarget_updateViewport();
	LatteRenderTarget_updateScissorBox();

	// rasterization kill only counts as such while the viewport X offset is enabled. Games enable it
	// via special state 0 and still expect depth output otherwise
	const bool rasterKill = gpu.contextNew.PA_CL_CLIP_CNTL.get_DX_RASTERIZATION_KILL() && gpu.contextNew.PA_CL_VTE_CNTL.get_VPORT_X_OFFSET_ENA();
	if (rasterKill && !streamoutActive)
		m_state.drawSequenceSkip = true;
}

bool VulkanRenderer::DrawHandleSpecialStates()
{
	const auto& special = LatteGPUState.contextNew.GetSpecialStateValues();
	if (special[8] != 0)
	{
		LatteDraw_handleSpecialState8_clearAsDepth();
		return true;
	}
	if (special[5] != 0)
	{
		draw_handleSpecialState5();
		return true;
	}
	return false;
}

void VulkanRenderer::DrawPrepareIndices(MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, uint32 count, uint32& indexMax, Renderer::INDEX_TYPE& hostIndexType, uint32& hostIndexCount)
{
	indexMax = 0;
	Renderer::IndexAllocation allocation;
	const auto primitiveMode = (LattePrimitiveMode)LatteGPUState.contextRegister[mmVGT_PRIMITIVE_TYPE];
	LatteIndices_decode(memory_getPointerFromVirtualOffset(indexDataMPTR), indexType, count, primitiveMode, indexMax, hostIndexType, hostIndexCount, allocation);
	if (hostIndexType == INDEX_TYPE::NONE)
		return;

	auto* reservation = (VKRSynchronizedHeapAllocator::AllocatorReservation*)allocation.rendererInternal;
	const bool sameBinding = m_state.activeIndexType == hostIndexType && m_state.activeIndexBufferIndex == reservation->bufferIndex && m_state.activeIndexBufferOffset == reservation->bufferOffset;
	if (sameBinding)
		return;
	m_state.activeIndexType = hostIndexType;
	m_state.activeIndexBufferIndex = reservation->bufferIndex;
	m_state.activeIndexBufferOffset = reservation->bufferOffset;

	VkIndexType vkIndexType = VK_INDEX_TYPE_UINT32;
	if (hostIndexType == INDEX_TYPE::U16)
		vkIndexType = VK_INDEX_TYPE_UINT16;
	else
		cemu_assert(hostIndexType == INDEX_TYPE::U32);
	vkCmdBindIndexBuffer(m_state.currentCommandBuffer, reservation->vkBuffer, reservation->bufferOffset, vkIndexType);
}

void VulkanRenderer::DrawUpdateDirectAccessBuffers()
{
	draw_updateVertexBuffersDirectAccess();
	if (LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader())
		draw_updateUniformBuffersDirectAccess(vs, mmSQ_VTX_UNIFORM_BLOCK_START, LatteConst::ShaderType::Vertex);
	if (LatteDecompilerShader* gs = LatteSHRC_GetActiveGeometryShader())
		draw_updateUniformBuffersDirectAccess(gs, mmSQ_GS_UNIFORM_BLOCK_START, LatteConst::ShaderType::Geometry);
	if (LatteDecompilerShader* ps = LatteSHRC_GetActivePixelShader())
		draw_updateUniformBuffersDirectAccess(ps, mmSQ_PS_UNIFORM_BLOCK_START, LatteConst::ShaderType::Pixel);
}

void VulkanRenderer::DrawBindPipeline(PipelineInfo* pipelineInfo)
{
	VKRObjectPipeline* pipelineObj = pipelineInfo->m_vkrObjPipeline;
	const VkPipeline pipeline = pipelineObj->GetPipeline();
	const bool newPipeline = m_state.currentPipeline != pipeline;
	if (newPipeline)
	{
		vkCmdBindPipeline(m_state.currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		pipelineObj->flagForCurrentCommandBuffer();
		m_state.currentPipeline = pipeline;
	}
	// a freshly bound pipeline needs the bias set even if the registers did not change
	if (pipelineInfo->usesDepthBias)
		draw_updateDepthBias(newPipeline);
	if (pipelineInfo->usesBlendConstants)
		draw_updateVkBlendConstants();
}

void VulkanRenderer::DrawBindDescriptorSet(PipelineInfo* pipelineInfo, uint32 firstSet, VkDescriptorSetInfo* first, uint32 firstStage, VkDescriptorSetInfo* second, uint32 secondStage)
{
	// binds one set, or two consecutive sets in a single call
	uint32 offsets[2 * 17];
	sint32 offsetCount = 0;
	VkDescriptorSet sets[2];
	uint32 setCount = 0;
	sets[setCount++] = first->m_vkObjDescriptorSet->descriptorSet;
	draw_prepareDynamicOffsetsForDescriptorSet(firstStage, offsets, offsetCount, pipelineInfo);
	if (second)
	{
		sint32 secondCount = 0;
		sets[setCount++] = second->m_vkObjDescriptorSet->descriptorSet;
		draw_prepareDynamicOffsetsForDescriptorSet(secondStage, offsets + offsetCount, secondCount, pipelineInfo);
		offsetCount += secondCount;
	}
	vkCmdBindDescriptorSets(m_state.currentCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineInfo->m_vkrObjPipeline->m_pipelineLayout, firstSet, setCount, sets, (uint32)offsetCount, offsets);
}

void VulkanRenderer::DrawIssue(Renderer::INDEX_TYPE hostIndexType, uint32 hostIndexCount, uint32 count, uint32 instanceCount, uint32 baseVertex, uint32 baseInstance)
{
	if (hostIndexType == INDEX_TYPE::NONE)
		vkCmdDraw(m_state.currentCommandBuffer, count, instanceCount, baseVertex, baseInstance);
	else
		vkCmdDrawIndexed(m_state.currentCommandBuffer, hostIndexCount, instanceCount, 0, baseVertex, baseInstance);
	LatteStreamout_FinishDrawcall(m_useHostMemoryForCache);
}

void VulkanRenderer::draw_execute_first(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext)
{
	if (m_state.drawSequenceSkip || DrawHandleSpecialStates())
		return;

	m_streamoutState.verticesPerInstance = count;
	LatteStreamout_PrepareDrawcall(count, instanceCount);

	// the first draw of a sequence uploads all uniform data
	if (LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader())
		uniformData_updateUniformVars(VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX, vs, s_vsUniformStaging);
	if (LatteDecompilerShader* ps = LatteSHRC_GetActivePixelShader())
		uniformData_updateUniformVars(VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT, ps, s_psUniformStaging);
	if (LatteDecompilerShader* gs = LatteSHRC_GetActiveGeometryShader())
		uniformData_updateUniformVars(VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY, gs, s_gsUniformStaging);
	m_cmdBufferUniformRingbufIndices[m_commandBufferIndex] = m_uniformVarBufferWriteIndex;

	uint32 indexMax;
	Renderer::INDEX_TYPE hostIndexType;
	uint32 hostIndexCount;
	DrawPrepareIndices(indexDataMPTR, indexType, count, indexMax, hostIndexType, hostIndexCount);

	if (m_useHostMemoryForCache)
	{
		DrawUpdateDirectAccessBuffers();
	}
	else
	{
		uint8 modifiedStages = 0;
		LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, modifiedStages);
	}

	PipelineInfo* pipelineInfo = draw_getOrCreateGraphicsPipeline(count);
	m_state.activePipelineInfo = pipelineInfo;
	if (pipelineInfo->m_vkrObjPipeline->GetPipeline() == VK_NULL_HANDLE)
	{
		// still compiling or failed
		m_state.activeVertexDS = nullptr;
		return;
	}

	VkDescriptorSetInfo* vsSet = nullptr;
	VkDescriptorSetInfo* psSet = nullptr;
	VkDescriptorSetInfo* gsSet = nullptr;
	draw_prepareDescriptorSets(pipelineInfo, vsSet, psSet, gsSet);
	m_state.activeVertexDS = vsSet;
	m_state.activePixelDS = psSet;
	m_state.activeGeometryDS = gsSet;
	m_state.descriptorSetsChanged = true;

	draw_setRenderPass();
	DrawBindPipeline(pipelineInfo);

	// set layout: 0 = vertex, 1 = pixel, 2 = geometry
	if (vsSet)
		DrawBindDescriptorSet(pipelineInfo, 0, vsSet, VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX, psSet, VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT);
	else if (psSet)
		DrawBindDescriptorSet(pipelineInfo, 1, psSet, VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT, nullptr, 0);
	if (gsSet)
		DrawBindDescriptorSet(pipelineInfo, 2, gsSet, VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY, nullptr, 0);

	DrawIssue(hostIndexType, hostIndexCount, count, instanceCount, baseVertex, baseInstance);
}

void VulkanRenderer::draw_execute_continued(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext)
{
	if (m_state.drawSequenceSkip || DrawHandleSpecialStates())
		return;

	m_streamoutState.verticesPerInstance = count;
	LatteStreamout_PrepareDrawcall(count, instanceCount);

	// follow-up draws only upload what the command stream changed
	LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader();
	LatteDecompilerShader* ps = LatteSHRC_GetActivePixelShader();
	LatteDecompilerShader* gs = LatteSHRC_GetActiveGeometryShader();
	uint8 modifiedStages = 0; // bit per SHADER_STAGE_INDEX_*
	if (vs)
		uniformData_updateUniformVarsIncremental(VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX, vs, modifiedStages, s_vsUniformStaging, drawcallContext.aluConstVSDirty, drawcallContext.vsUniformBufferDirtyMask);
	if (ps)
		uniformData_updateUniformVarsIncremental(VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT, ps, modifiedStages, s_psUniformStaging, drawcallContext.aluConstPSDirty, drawcallContext.psUniformBufferDirtyMask);
	if (gs)
		uniformData_updateUniformVarsIncremental(VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY, gs, modifiedStages, s_gsUniformStaging, false, drawcallContext.gsUniformBufferDirtyMask);
	m_cmdBufferUniformRingbufIndices[m_commandBufferIndex] = m_uniformVarBufferWriteIndex;

	uint32 indexMax;
	Renderer::INDEX_TYPE hostIndexType;
	uint32 hostIndexCount;
	DrawPrepareIndices(indexDataMPTR, indexType, count, indexMax, hostIndexType, hostIndexCount);

	if (m_useHostMemoryForCache)
		DrawUpdateDirectAccessBuffers();
	else
		LatteBufferCache_Sync(indexMax + baseVertex, baseInstance, instanceCount, drawcallContext.vertexBufferDirtyMask, drawcallContext.vsUniformBufferDirtyMask, drawcallContext.psUniformBufferDirtyMask, drawcallContext.gsUniformBufferDirtyMask, modifiedStages, true);

	// only the minimal state can change between draws of a sequence
	m_state.descriptorSetsChanged = false;
	PipelineInfo* pipelineInfo = m_state.activePipelineInfo;
	const uint64 minimalHash = draw_calculateMinimalGraphicsPipelineHash(vs->compatibleFetchShader, LatteGPUState.contextNew);
	if (pipelineInfo->minimalStateHash != minimalHash) [[unlikely]]
	{
		pipelineInfo = draw_getOrCreateGraphicsPipeline(count);
		m_state.activePipelineInfo = pipelineInfo;
		draw_prepareDescriptorSets(pipelineInfo, m_state.activeVertexDS, m_state.activePixelDS, m_state.activeGeometryDS);
		m_state.descriptorSetsChanged = true;
		modifiedStages = 0x7; // new sets, rebind everything
	}
	else
	{
#ifdef CEMU_DEBUG_ASSERT
		cemu_assert_debug(pipelineInfo == draw_getOrCreateGraphicsPipeline(count));
#endif
	}

	if (pipelineInfo->m_vkrObjPipeline->GetPipeline() == VK_NULL_HANDLE)
		return;

	VkDescriptorSetInfo* vsSet = nullptr;
	VkDescriptorSetInfo* psSet = nullptr;
	VkDescriptorSetInfo* gsSet = nullptr;
	if (m_state.activeVertexDS)
	{
		vsSet = m_state.activeVertexDS;
		psSet = m_state.activePixelDS;
		gsSet = m_state.activeGeometryDS;
	}
	else
	{
		cemu_assert(false);
	}

	draw_setRenderPass();
	DrawBindPipeline(pipelineInfo);

	// rebind the vertex/pixel sets only for stages whose uniform data moved
	const bool rebindVS = vsSet && (modifiedStages & (1 << VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX));
	const bool rebindPS = psSet && (modifiedStages & (1 << VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT));
	if (rebindVS)
		DrawBindDescriptorSet(pipelineInfo, 0, vsSet, VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX, rebindPS ? psSet : nullptr, VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT);
	else if (rebindPS)
		DrawBindDescriptorSet(pipelineInfo, 1, psSet, VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT, nullptr, 0);
	if (gsSet)
		DrawBindDescriptorSet(pipelineInfo, 2, gsSet, VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY, nullptr, 0);

	DrawIssue(hostIndexType, hostIndexCount, count, instanceCount, baseVertex, baseInstance);
}

void VulkanRenderer::draw_execute(uint32 baseVertex, uint32 baseInstance, uint32 instanceCount, uint32 count, MPTR indexDataMPTR, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, const LatteDrawcallContext& drawcallContext)
{
	if (drawcallContext.isFirst)
		draw_execute_first(baseVertex, baseInstance, instanceCount, count, indexDataMPTR, indexType, drawcallContext);
	else
		draw_execute_continued(baseVertex, baseInstance, instanceCount, count, indexDataMPTR, indexType, drawcallContext);
	m_state.colorBufferSyncPending = false;
	LatteGPUState.drawCallCounter++;
}

// with the guest memory imported as one Vulkan buffer, bindings are offsets into that buffer
void VulkanRenderer::draw_updateVertexBuffersDirectAccess()
{
	LatteFetchShader* fetchShader = LatteSHRC_GetActiveFetchShader();
	if (fetchShader == nullptr)
		return;
	for (const auto& group : fetchShader->bufferGroups)
	{
		const uint32 slot = group.attributeBufferIndex;
		MPTR address = LatteGPUState.contextRegister[mmSQ_VTX_ATTRIBUTE_BLOCK_START + slot * 7];
		if (address == MPTR_NULL) [[unlikely]]
			address = 0x10000000;
		// note: currentVertexBinding is not written on this path, so in practice every group is bound
		if (m_state.currentVertexBinding[slot].offset == address)
			continue;
		cemu_assert_debug(address < 0x50000000);
		VkBuffer buffer = m_importedMem;
		VkDeviceSize offset = address - m_importedMemBaseAddress;
		vkCmdBindVertexBuffers(m_state.currentCommandBuffer, slot, 1, &buffer, &offset);
	}
}

void VulkanRenderer::draw_updateUniformBuffersDirectAccess(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset, LatteConst::ShaderType shaderType)
{
	if (shader->uniformMode != LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
		return;
	uint32 stage = 0;
	switch (shaderType)
	{
	case LatteConst::ShaderType::Vertex: stage = VulkanRendererConst::SHADER_STAGE_INDEX_VERTEX; break;
	case LatteConst::ShaderType::Pixel: stage = VulkanRendererConst::SHADER_STAGE_INDEX_FRAGMENT; break;
	case LatteConst::ShaderType::Geometry: stage = VulkanRendererConst::SHADER_STAGE_INDEX_GEOMETRY; break;
	default: UNREACHABLE;
	}
	for (const auto& ub : shader->list_quickBufferList)
	{
		const uint32 slot = ub.index;
		const MPTR address = LatteGPUState.contextRegister[uniformBufferRegOffset + slot * 7];
		if (address == MPTR_NULL) [[unlikely]]
		{
			cemu_assert_unimplemented();
			continue;
		}
		cemu_assert_debug(address < 0x50000000);
		cemu_assert_debug(slot < 16);
		dynamicOffsetInfo.shaderUB[stage].uniformBufferOffset[slot] = address - m_importedMemBaseAddress;
	}
}

void VulkanRenderer::draw_endSequence()
{
	if (LatteSHRC_GetActivePixelShader())
		LatteRenderTarget_trackUpdates();
	const bool readbackPending = LatteTextureReadback_Update();
	m_recordedDrawcalls++;
	if (readbackPending || m_recordedDrawcalls >= m_submitThreshold)
		SubmitCommandBuffer();
}

void VulkanRenderer::debug_genericBarrier()
{
	draw_endRenderPass();
	constexpr VkAccessFlags everyAccess =
		VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
		VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
		VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
		VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT |
		VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	VkMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = everyAccess;
	barrier.dstAccessMask = everyAccess;
	vkCmdPipelineBarrier(m_state.currentCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}
