// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif

// Vertex and uniform buffer setup for a draw: guest buffers are looked up in (and
// if needed loaded into) the GPU buffer cache and bound by host buffer offset.

namespace
{
	// each vertex/uniform buffer occupies 7 consecutive resource registers
	constexpr uint32 kBufferResourceStride = 7;

	inline void Copy16(uint8* dst, const void* src) { memcpy(dst, src, 16); }

	inline uint32 UniformBlockRegisterBase(LatteConst::ShaderType type)
	{
		switch (type)
		{
		case LatteConst::ShaderType::Vertex:
			return mmSQ_VTX_UNIFORM_BLOCK_START;
		case LatteConst::ShaderType::Pixel:
			return mmSQ_PS_UNIFORM_BLOCK_START;
		case LatteConst::ShaderType::Geometry:
			return mmSQ_GS_UNIFORM_BLOCK_START;
		default:
			UNREACHABLE;
		}
	}

	inline uint32 StageBit(LatteConst::ShaderType type)
	{
		return 1u << static_cast<uint32>(type);
	}
}

// Fills the remapped uniform array of a shader (compact layout used when the host
// cannot bind full constant banks). Returns true if anything was written.
bool LatteBufferCache_LoadRemappedUniforms(LatteDecompilerShader* shader, float* uniformData, bool aluConstDirty, uint32 uniformBufferDirtyMask)
{
	uint8* out = reinterpret_cast<uint8*>(uniformData);
	if (LatteGPUState.contextNew.SQ_CONFIG.get_DX9_CONSTS())
	{
		// constants come from the ALU constant registers; the vertex shader's bank starts 0x400 registers in
		uint32 bankStart;
		switch (shader->shaderType)
		{
		case LatteConst::ShaderType::Vertex:
			bankStart = 0x400;
			break;
		case LatteConst::ShaderType::Pixel:
		case LatteConst::ShaderType::Geometry:
			bankStart = 0;
			break;
		default:
			UNREACHABLE;
		}
		if (!aluConstDirty)
			return false;
		const uint32* bank = LatteGPUState.contextRegister + mmSQ_ALU_CONSTANT0_0 + bankStart;
		for (const auto& entry : shader->list_remappedUniformEntries_register)
			Copy16(out + entry.mappedIndexOffset, bank + entry.indexOffset / 4);
		return !shader->list_remappedUniformEntries_register.empty();
	}

	// constants come from uniform buffers in guest memory
	const uint32 blockRegs = UniformBlockRegisterBase(shader->shaderType);
	if (uniformBufferDirtyMask == 0)
		return false;
	bool wrote = false;
	for (const auto& group : shader->list_remappedUniformEntries_bufferGroups)
	{
		if ((uniformBufferDirtyMask & (1u << group.bufferId)) == 0)
			continue;
		const MPTR bufferAddr = LatteGPUState.contextRegister[blockRegs + group.kcacheBankIdOffset / 4];
		for (const auto& entry : group.entries)
		{
			if (bufferAddr != MPTR_NULL)
				Copy16(out + entry.mappedIndexOffset, memory_base + bufferAddr + entry.indexOffset);
			else
				memset(out + entry.mappedIndexOffset, 0, 16);
		}
		wrote = true;
	}
	return wrote;
}

// Binds the dirty uniform buffers of a shader that reads whole constant banks.
// Returns true if any of the shader's buffers was dirty.
static bool LatteBufferCache_syncGPUUniformBuffers(LatteDecompilerShader* shader, const uint32 uniformBufferRegOffset, LatteConst::ShaderType shaderType, uint32 bufferDirtyMask)
{
	cemu_assert_debug(shader->uniformMode == LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK);
	bool anyDirty = false;
	for (const auto& buf : shader->list_quickBufferList)
	{
		const uint32 index = buf.index;
		if ((bufferDirtyMask & (1u << index)) == 0)
			continue;
		anyDirty = true;
		const uint32* regs = LatteGPUState.contextRegister + uniformBufferRegOffset + index * kBufferResourceStride;
		const MPTR addr = regs[0];
		if (addr == MPTR_NULL) [[unlikely]]
		{
			g_renderer->buffer_bindUniformBuffer(shaderType, index, 0, 0);
			continue;
		}
		// bind no more than the shader actually reads
		const uint32 size = std::min<uint32>(regs[1] + 1, buf.size);
		g_renderer->buffer_bindUniformBuffer(shaderType, index, LatteBufferCache_retrieveDataInCache(addr, size), size);
	}
	return anyDirty;
}

// highest vertex index / instance covered by the current vertex buffer bindings (incremental draws)
static uint32 s_boundMaxVertex{};
static uint32 s_boundMaxInstance{};

// cache housekeeping between (non-incremental) draws
static void LatteBufferCache_ProcessQueues()
{
	static uint32 s_drawsSinceCleanup = 0;
	if (++s_drawsSinceCleanup >= 25)
	{
		LatteBufferCache_incrementalCleanup();
		s_drawsSinceCleanup = 0;
	}
	LatteBufferCache_processDCFlushQueue();
	LatteBufferCache_processDeallocations(); // allocations released by the previous draw
}

void LatteBufferCache_Sync(uint32 maxVtxIndex, uint32 baseInstance, uint32 instanceCount, uint32 attribBufferDirtyMask, uint32 vsUniformBufferDirtyMask, uint32 psUniformBufferDirtyMask, uint32 gsUniformBufferDirtyMask, uint8& stageUniformModifiedMask, bool isIncremental)
{
	LatteFetchShader* fetchShader = LatteSHRC_GetActiveFetchShader();
	cemu_assert_debug(fetchShader);
	const uint32 maxInstance = baseInstance + instanceCount - 1;

	if (isIncremental)
	{
		// Part of a run of draws that keep their bindings: housekeeping is skipped and
		// buffers are only rebound when the draw reaches past the bound vertex/instance range
		const bool bindingsChanged = attribBufferDirtyMask != 0;
		if (maxVtxIndex > s_boundMaxVertex)
		{
			attribBufferDirtyMask = 0xFFFFFFFF;
			s_boundMaxVertex = maxVtxIndex;
		}
		if (maxInstance > s_boundMaxInstance)
		{
			attribBufferDirtyMask = 0xFFFFFFFF;
			s_boundMaxInstance = maxInstance;
		}
		if (bindingsChanged)
		{
			s_boundMaxVertex = maxVtxIndex;
			s_boundMaxInstance = maxInstance;
		}
	}
	else
	{
		LatteBufferCache_ProcessQueues();
		s_boundMaxVertex = maxVtxIndex;
		s_boundMaxInstance = maxInstance;
	}
	attribBufferDirtyMask &= fetchShader->attributeBufferMask;

	if (attribBufferDirtyMask != 0)
	{
		const uint32* attribRegs = LatteGPUState.contextRegister + mmSQ_VTX_ATTRIBUTE_BLOCK_START;
		Renderer::BindBufferParam bindings[Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS];
		size_t bindingCount = 0;
		cemu_assert_debug(fetchShader->bufferGroups.size() < Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS);
		for (const auto& group : fetchShader->bufferGroups)
		{
			const uint32 bufferIndex = group.attributeBufferIndex;
			if ((attribBufferDirtyMask & (1u << bufferIndex)) == 0)
				continue;
			const uint32* regs = attribRegs + bufferIndex * kBufferResourceStride;
			const MPTR addr = regs[0];
			const uint32 stride = (regs[2] >> 11) & 0xFFFF;
			Renderer::BindBufferParam& bind = bindings[bindingCount];
			bind.stride = stride;
			if (addr == MPTR_NULL) [[unlikely]]
			{
				bind.index = bufferIndex;
				bind.bindOffset = 0;
				bind.bindSize = 0;
				bindingCount++;
				continue;
			}
			// The size the game set for the buffer can be far larger than what the draw
			// reads, so only the range reachable by the highest index is cached.
			const uint32 lastElement = std::max<uint32>(group.hasVtxIndexAccess ? maxVtxIndex : 0, group.hasInstanceIndexAccess ? maxInstance : 0);
			const uint32 accessedSize = (stride * lastElement + group.totalAttribRangeSize + 127) & ~127u;
			const uint32 lookupSize = std::max<uint32>(accessedSize, 1);
#if BOOST_OS_MACOS && defined(ENABLE_VULKAN)
			// MoltenVK needs 4 byte aligned strides: such buffers are repacked
			if (stride % 4 != 0 && g_renderer->GetType() == RendererAPI::Vulkan)
			{
				if (VulkanRenderer* vk = VulkanRenderer::GetInstance())
				{
					auto repacked = vk->buffer_genStrideWorkaroundVertexBuffer(addr, accessedSize, stride);
					vk->buffer_bindVertexStrideWorkaroundBuffer(repacked.first, repacked.second, bufferIndex, accessedSize);
					continue;
				}
			}
#endif
			bind.index = bufferIndex;
			bind.bindOffset = LatteBufferCache_retrieveDataInCache(addr, lookupSize);
			bind.bindSize = accessedSize;
			bindingCount++;
		}
		g_renderer->buffer_bindVertexBuffers({ bindings, bindingCount });
	}

	struct StageUniforms
	{
		LatteDecompilerShader* shader;
		uint32 dirtyMask;
		LatteConst::ShaderType type;
	};
	const StageUniforms stages[] = {
		{ LatteSHRC_GetActiveVertexShader(), vsUniformBufferDirtyMask, LatteConst::ShaderType::Vertex },
		{ LatteSHRC_GetActivePixelShader(), psUniformBufferDirtyMask, LatteConst::ShaderType::Pixel },
		{ LatteSHRC_GetActiveGeometryShader(), gsUniformBufferDirtyMask, LatteConst::ShaderType::Geometry },
	};
	for (const auto& stage : stages)
	{
		if (!stage.shader || stage.shader->uniformMode != LATTE_DECOMPILER_UNIFORM_MODE_FULL_CBANK)
			continue;
		if (LatteBufferCache_syncGPUUniformBuffers(stage.shader, UniformBlockRegisterBase(stage.type), stage.type, stage.dirtyMask))
			stageUniformModifiedMask |= (uint8)StageBit(stage.type);
	}
}
