// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/ActiveSettings.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "util/containers/flat_hash_map.hpp"
#include "util/containers/robin_hood.h"
#include "util/helpers/StateHasher.h"
#include "util/helpers/StringBuf.h"

// Shader management. Each stage's program is identified by a base hash (program words
// plus the bits of GPU state the generated code depends on) and an aux hash (state that
// only some programs care about, such as texture types). Shaders with the same base hash
// form a chain through LatteDecompilerShader::next. On top of that a "shader state" cache
// maps a hash of the raw program pointers and relevant registers straight to the active
// set of shaders, so most draws skip the per-stage lookups entirely.
//
// The base hashes are part of the shader cache entry names and of graphic pack shader
// file names, so the hash formulas below must never change.

uint64 _shaderBaseHash_vs;
uint64 _shaderBaseHash_gs;
uint64 _shaderBaseHash_ps;

std::atomic_int g_compiled_shaders_total = 0;
std::atomic_int g_compiled_shaders_async = 0;
std::atomic_int g_shaderStateCacheSetCount = 0;
std::atomic_int g_shaderStateCacheSetAuxCount = 0;

void LatteSHRC_RemoveShaderStateCacheEntryByKey(uint64 key);

namespace
{
	// remembers the hash of the last program seen at a pointer/size so it is not rehashed every draw
	struct ProgramHashMemo
	{
		const uint32* code;
		uint32 size;
		uint64 sum;
		uint64 mix;
	};

	ProgramHashMemo s_memoVS{};
	ProgramHashMemo s_memoGS{};
	ProgramHashMemo s_memoPS{};

	// the two running hashes of a program: rotating sum and rotating xor of all words
	void HashProgramWords(const uint32* words, uint32 byteSize, uint64& sum, uint64& mix)
	{
		sum = 0;
		mix = 0;
		const uint32 wordCount = byteSize / 4;
		for (uint32 i = 0; i < wordCount; i++)
		{
			sum = std::rotl<uint64>(sum + words[i], 3);
			mix = std::rotr<uint64>(mix ^ words[i], 7);
		}
	}

	void HashProgram(ProgramHashMemo& memo, const uint32* words, uint32 byteSize, uint64& sum, uint64& mix)
	{
		if (words == nullptr)
		{
			memo = {};
			sum = 0;
			mix = 0;
			return;
		}
		if (memo.code != words || memo.size != byteSize)
		{
			HashProgramWords(words, byteSize, memo.sum, memo.mix);
			memo.code = words;
			memo.size = byteSize;
		}
		sum = memo.sum;
		mix = memo.mix;
	}

	using ShaderTable = ska::flat_hash_map<uint64, LatteDecompilerShader*>;
	ShaderTable s_vsTable(512);
	ShaderTable s_gsTable(512);
	ShaderTable s_psTable(512);

	ShaderTable& TableFor(LatteConst::ShaderType type)
	{
		switch (type)
		{
		case LatteConst::ShaderType::Vertex:
			return s_vsTable;
		case LatteConst::ShaderType::Geometry:
			return s_gsTable;
		default:
			cemu_assert_debug(type == LatteConst::ShaderType::Pixel);
			return s_psTable;
		}
	}

	LatteDecompilerShader* FindInChain(LatteDecompilerShader* chain, uint64 auxHash)
	{
		for (; chain; chain = chain->next)
		{
			if (chain->auxHash == auxHash)
				return chain;
		}
		return nullptr;
	}

	LatteDecompilerShader* Lookup(ShaderTable& table, uint64 baseHash, uint64 auxHash)
	{
		auto it = table.find(baseHash);
		if (it == table.end())
			return nullptr;
		return FindInChain(it->second, auxHash);
	}

	// unlinks a shader from its chain; the chain head stays in the table
	void Unregister(LatteDecompilerShader* shader)
	{
		ShaderTable& table = TableFor(shader->shaderType);
		bool found = false;
		auto it = table.find(shader->baseHash);
		if (it == table.end())
		{
			cemu_assert_suspicious();
		}
		else if (it->second == shader)
		{
			LatteDecompilerShader* successor = shader->next;
			table.erase(it);
			if (successor)
			{
				cemu_assert_debug(successor->baseHash == shader->baseHash);
				table.emplace(shader->baseHash, successor);
			}
			shader->next = nullptr;
			found = true;
		}
		else
		{
			for (LatteDecompilerShader* prev = it->second; prev->next; prev = prev->next)
			{
				if (prev->next == shader)
				{
					prev->next = shader->next;
					found = true;
					break;
				}
			}
		}
		cemu_assert(found);
		// every state cache entry that references the shader goes too (with all its aux variants)
		while (!shader->m_shaderStateCacheKeys.empty())
		{
			const uint64 key = shader->m_shaderStateCacheKeys.back();
			LatteSHRC_RemoveShaderStateCacheEntryByKey(key);
			if (!shader->m_shaderStateCacheKeys.empty() && shader->m_shaderStateCacheKeys.back() == key)
				shader->m_shaderStateCacheKeys.pop_back(); // stale key, entry already gone
		}
	}

	LatteFetchShader* s_activeFS = nullptr;
	LatteDecompilerShader* s_activeVS = nullptr;
	LatteDecompilerShader* s_activeGS = nullptr;
	LatteDecompilerShader* s_activePS = nullptr;

	LatteShaderPSInputTable s_psInputs;

	uint32 Reg(uint32 index)
	{
		return LatteGPUState.contextRegister[index];
	}

	uint64 VertexShaderBaseHash(uint8* program, uint32 programSize, bool usesGeometryShader, LatteFetchShader* fetchShader)
	{
		uint64 sum, mix;
		HashProgram(s_memoVS, (const uint32*)program, programSize, sum, mix);
		uint64 h = sum + mix + fetchShader->key + s_psInputs.key;
		if (usesGeometryShader)
			h += 0x1111;
		// viewport transform enables
		h += (uint64)(LatteGPUState.contextNew.PA_CL_VTE_CNTL.getRawValue() ^ 0x43F);
		const auto primitive = LatteGPUState.contextNew.VGT_PRIMITIVE_TYPE.get_PRIMITIVE_MODE();
		if (primitive == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::RECTS)
			h += 13;
		else if (primitive == Latte::LATTE_VGT_PRIMITIVE_TYPE::E_PRIMITIVE_TYPE::POINTS)
			h += 71; // point size is written by the vertex shader
		if (Reg(mmVGT_STRMOUT_EN))
			h += 21;
		if (LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF())
			h += 0x1537; // depth range 0..1
		return h;
	}

	uint64 GeometryShaderBaseHash(uint8* program, uint32 programSize)
	{
		uint64 sum, mix;
		HashProgram(s_memoGS, (const uint32*)program, programSize, sum, mix);
		// uses the ring size of the vertex shader that was active before this update
		uint64 h = sum + mix + (uint64)s_activeVS->ringParameterCount;
		if (Reg(mmVGT_STRMOUT_EN))
			h += 21;
		return h;
	}

	uint64 PixelShaderBaseHash(uint8* program, uint32 programSize, bool usesGeometryShader)
	{
		uint64 sum, mix;
		HashProgram(s_memoPS, (const uint32*)program, programSize, sum, mix);
		uint64 h = sum + mix + s_psInputs.key;
		// the geometry program's sum hash from the last time it was hashed
		if (usesGeometryShader)
			h += s_memoGS.sum;
		return h;
	}

	uint64 VertexShaderAuxHash(LatteDecompilerShader* shader, const uint32* regs)
	{
		uint64 strides = 0;
		if (shader->hasStreamoutBufferWrite)
		{
			for (uint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
			{
				if (shader->streamoutBufferWriteMask[b])
					strides = std::rotl<uint64>(strides, 7) + regs[mmVGT_STRMOUT_VTX_STRIDE_0 + b * 4];
			}
		}
		// integer textures change the sampling code
		uint64 textures = 0;
		for (uint32 i = 0; i < shader->textureUnitListCount; i++)
		{
			const uint32 unit = shader->textureUnitList[i];
			const uint32 word4 = regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS + unit * 7 + 4];
			if ((word4 & 0x300) == 0x100)
				textures = std::rotl<uint64>(textures, 7) + 0x333;
		}
		return strides + textures;
	}

	uint64 PixelShaderAuxHash(LatteDecompilerShader* shader, const uint32* regs)
	{
		// output remapping
		uint64 h = regs[mmCB_SHADER_MASK];
		// alpha test function
		const uint32 alphaTest = regs[Latte::REGADDR::SX_ALPHA_TEST_CONTROL];
		if ((alphaTest >> 3) & 1)
			h = std::rotr<uint64>(h + (alphaTest & 7), 3) + 1;
		// texture dimensions
		for (uint32 i = 0; i < shader->textureUnitListCount; i++)
		{
			const uint32 unit = shader->textureUnitList[i];
			const uint32 word0 = regs[Latte::REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + unit * 7];
			h = std::rotl<uint64>(h, 3) + (word0 & 7);
		}
		return h;
	}

	uint64 GeometryShaderAuxHash(LatteDecompilerShader*)
	{
		return 0;
	}

	void CopyUniformLayout(LatteDecompilerShader* shader, const LatteDecompilerOutput_t& output)
	{
		if (g_renderer->GetType() == RendererAPI::OpenGL)
		{
			// the OpenGL backend looks the locations up itself (LatteShader_prepareSeparableUniforms)
			shader->uniform.count_uniformRegister = output.uniformOffsetsGL.count_uniformRegister;
			return;
		}
		const LatteDecompilerOutputUniformOffsets& src = output.uniformOffsetsVK;
		auto& dst = shader->uniform;
		dst.loc_remapped = src.offset_remapped;
		dst.loc_uniformRegister = src.offset_uniformRegister;
		dst.count_uniformRegister = src.count_uniformRegister;
		dst.loc_windowSpaceToClipSpaceTransform = src.offset_windowSpaceToClipSpaceTransform;
		dst.loc_alphaTestRef = src.offset_alphaTestRef;
		dst.loc_pointSize = src.offset_pointSize;
		dst.loc_fragCoordScale = src.offset_fragCoordScale;
		dst.loc_verticesPerInstance = src.offset_verticesPerInstance;
		dst.uniformRangeSize = src.offset_endOfBlock;
		dst.list_ufTexRescale.clear();
		for (sint32 unit = 0; unit < LATTE_NUM_MAX_TEX_UNITS; unit++)
		{
			dst.loc_framebufferFetchSize[unit] = src.offset_framebufferFetchSize[unit];
			if (src.offset_texScale[unit] >= 0)
			{
				LatteUniformTextureScaleEntry_t scale{};
				scale.texUnit = unit;
				scale.uniformLocation = src.offset_texScale[unit];
				dst.list_ufTexRescale.push_back(scale);
			}
		}
		for (sint32 b = 0; b < LATTE_NUM_STREAMOUT_BUFFER; b++)
			dst.loc_streamoutBufferBase[b] = src.offset_streamoutBufferBase[b];
	}

	// OpenGL needs the program linked before uniforms can be queried, so it is not left in the background
	void FinishNowOnOpenGL(LatteDecompilerShader* shader)
	{
#ifdef ENABLE_OPENGL
		if (g_renderer->GetType() != RendererAPI::OpenGL)
			return;
		if (shader->shader)
			shader->shader->PreponeCompilation(true);
		LatteShader_FinishCompilation(shader);
#endif
	}

	LatteDecompilerShader* CompileVertexShader(uint64 baseHash, uint64& auxHash, uint8* program, uint32 programSize, bool usesGeometryShader, LatteFetchShader* fetchShader)
	{
		uint32* regs = LatteGPUState.contextRegister;
		LatteDecompilerOptions options;
		LatteShader_GetDecompilerOptions(options, LatteConst::ShaderType::Vertex, usesGeometryShader);
		LatteDecompilerOutput_t output{};
		LatteDecompiler_DecompileVertexShader(_shaderBaseHash_vs, regs, program, programSize, fetchShader, options, &output);
		LatteDecompilerShader* shader = LatteShader_CreateShaderFromDecompilerOutput(output, baseHash, true, 0, regs);
		auxHash = shader->auxHash;
		if (!shader->hasError)
		{
			uint8* fetchProgram = (uint8*)memory_getPointerFromPhysicalOffset(regs[mmSQ_PGM_START_FS] << 8);
			const uint32 fetchProgramSize = regs[mmSQ_PGM_START_FS + 1] << 3;
			LatteShaderCache_writeSeparableVertexShader(shader->baseHash, shader->auxHash, fetchProgram, fetchProgramSize, program, programSize, regs, usesGeometryShader);
		}
		LatteShader_DumpShader(shader->baseHash, shader->auxHash, shader);
		LatteShader_DumpRawShader(shader->baseHash, shader->auxHash, SHADER_DUMP_TYPE_VERTEX, program, programSize);
		LatteShader_CreateRendererShader(shader, false);
		performanceMonitor.numCompiledVS++;
		FinishNowOnOpenGL(shader);
		LatteSHRC_RegisterShader(shader, shader->baseHash, shader->auxHash);
		return shader;
	}

	LatteDecompilerShader* CompileGeometryShader(uint64 baseHash, uint8* program, uint32 programSize, uint8* copyProgram, uint32 copyProgramSize, LatteDecompilerShader* vertexShader)
	{
		uint32* regs = LatteGPUState.contextRegister;
		LatteDecompilerOptions options;
		LatteShader_GetDecompilerOptions(options, LatteConst::ShaderType::Geometry, true);
		LatteDecompilerOutput_t output{};
		LatteDecompiler_DecompileGeometryShader(_shaderBaseHash_gs, regs, program, programSize, copyProgram, copyProgramSize, vertexShader->ringParameterCount, options, &output);
		LatteDecompilerShader* shader = LatteShader_CreateShaderFromDecompilerOutput(output, baseHash, true, 0, regs);
		if (!shader->hasError)
			LatteShaderCache_writeSeparableGeometryShader(shader->baseHash, shader->auxHash, program, programSize, copyProgram, copyProgramSize, regs, LatteGPUState.contextNew.GetSpecialStateValues(), vertexShader->ringParameterCount);
		LatteShader_DumpShader(shader->baseHash, shader->auxHash, shader);
		LatteShader_DumpRawShader(shader->baseHash, shader->auxHash, SHADER_DUMP_TYPE_GEOMETRY, program, programSize);
		LatteShader_DumpRawShader(shader->baseHash, shader->auxHash, SHADER_DUMP_TYPE_COPY, copyProgram, copyProgramSize);
		LatteShader_CreateRendererShader(shader, false);
		performanceMonitor.numCompiledGS++;
		FinishNowOnOpenGL(shader);
		LatteSHRC_RegisterShader(shader, shader->baseHash, shader->auxHash);
		return shader;
	}

	LatteDecompilerShader* CompilePixelShader(uint64 baseHash, uint64& auxHash, uint8* program, uint32 programSize, bool usesGeometryShader)
	{
		uint32* regs = LatteGPUState.contextRegister;
		LatteDecompilerOptions options;
		LatteShader_GetDecompilerOptions(options, LatteConst::ShaderType::Pixel, usesGeometryShader);
		LatteDecompilerOutput_t output{};
		LatteDecompiler_DecompilePixelShader(baseHash, regs, program, programSize, options, &output);
		LatteDecompilerShader* shader = LatteShader_CreateShaderFromDecompilerOutput(output, baseHash, true, 0, regs);
		auxHash = shader->auxHash;
		LatteShader_DumpShader(_shaderBaseHash_ps, auxHash, shader);
		LatteShader_DumpRawShader(_shaderBaseHash_ps, auxHash, SHADER_DUMP_TYPE_PIXEL, program, programSize);
		// unlike the other stages the cache entry is only written once the host shader exists
		LatteShader_CreateRendererShader(shader, false);
		performanceMonitor.numCompiledPS++;
		if (!shader->hasError)
			LatteShaderCache_writeSeparablePixelShader(_shaderBaseHash_ps, auxHash, program, programSize, regs, usesGeometryShader);
		FinishNowOnOpenGL(shader);
		LatteSHRC_RegisterShader(shader, _shaderBaseHash_ps, auxHash);
		return shader;
	}

	LatteDecompilerShader* AcquireVertexShader(uint8* program, uint32 programSize, bool usesGeometryShader, LatteFetchShader* fetchShader)
	{
		_shaderBaseHash_vs = VertexShaderBaseHash(program, programSize, usesGeometryShader, fetchShader);
		uint64 auxHash = 0;
		LatteDecompilerShader* shader = nullptr;
		auto it = s_vsTable.find(_shaderBaseHash_vs);
		if (it != s_vsTable.end())
		{
			// any shader of the chain has the texture list needed for the aux hash
			auxHash = VertexShaderAuxHash(it->second, LatteGPUState.contextRegister);
			shader = FindInChain(it->second, auxHash);
		}
		if (!shader)
			shader = CompileVertexShader(_shaderBaseHash_vs, auxHash, program, programSize, usesGeometryShader, fetchShader);
		if (shader->hasError)
			LatteGPUState.activeShaderHasError = true;
		return shader;
	}

	LatteDecompilerShader* AcquireGeometryShader(bool usesGeometryShader, uint8* program, uint32 programSize, uint8* copyProgram, uint32 copyProgramSize, LatteDecompilerShader* vertexShader)
	{
		if (!usesGeometryShader || !s_activeVS)
		{
			_shaderBaseHash_gs = 0;
			return nullptr;
		}
		_shaderBaseHash_gs = GeometryShaderBaseHash(program, programSize);
		LatteDecompilerShader* shader;
		auto it = s_gsTable.find(_shaderBaseHash_gs);
		if (it != s_gsTable.end())
		{
			shader = it->second; // geometry shaders have no aux variants
			cemu_assert_debug(GeometryShaderAuxHash(shader) == 0);
		}
		else
			shader = CompileGeometryShader(_shaderBaseHash_gs, program, programSize, copyProgram, copyProgramSize, vertexShader);
		if (shader->hasError)
			LatteGPUState.activeShaderHasError = true;
		return shader;
	}

	LatteDecompilerShader* AcquirePixelShader(uint8* program, uint32 programSize, bool usesGeometryShader)
	{
		_shaderBaseHash_ps = PixelShaderBaseHash(program, programSize, usesGeometryShader);
		uint64 auxHash = 0;
		LatteDecompilerShader* shader = nullptr;
		auto it = s_psTable.find(_shaderBaseHash_ps);
		if (it != s_psTable.end())
		{
			auxHash = PixelShaderAuxHash(it->second, LatteGPUState.contextRegister);
			shader = FindInChain(it->second, auxHash);
		}
		if (!shader)
			shader = CompilePixelShader(_shaderBaseHash_ps, auxHash, program, programSize, usesGeometryShader);
		if (shader->hasError)
			LatteGPUState.activeShaderHasError = true;
		return shader;
	}

	// ---- shader state cache ----

	struct StageSet
	{
		uint64 auxHash{};
		LatteDecompilerShader* vs{};
		LatteDecompilerShader* ps{};
		LatteDecompilerShader* gs{};
		bool hasError{};
	};

	struct StateEntry
	{
		LatteFetchShader* fetchShader{};
		StageSet primary; // the set seen when the entry was created, checked first
		uint32 lastUsedFrame{};
		std::vector<StageSet> variants; // every set seen, including the primary one
	};

	// keys are already well mixed
	struct IdentityHash
	{
		size_t operator()(uint64 v) const noexcept { return (size_t)v; }
	};

	robin_hood::unordered_flat_map<uint64, StateEntry, IdentityHash> s_stateCache;
	std::vector<uint64> s_stateKeys; // walked a few entries per frame to expire unused ones
	size_t s_stateSweepPos = 0;

	uint64 Avalanche64(uint64 v)
	{
		v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
		v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
		return v ^ (v >> 31);
	}

	// combines the aux hashes the stage shaders of an entry would need right now
	uint64 CurrentAuxHash(LatteFetchShader* fetchShader, LatteDecompilerShader* vs, LatteDecompilerShader* ps)
	{
		const uint32* regs = LatteGPUState.contextRegister;
		uint64 h = (vs ? VertexShaderAuxHash(vs, regs) : 0) + Avalanche64(ps ? PixelShaderAuxHash(ps, regs) : 0);
		return h;
	}

	void Activate(LatteFetchShader* fetchShader, const StageSet& set)
	{
		s_activeFS = fetchShader;
		s_activeVS = set.vs;
		s_activePS = set.ps;
		s_activeGS = set.gs;
	}

	void LinkKey(LatteDecompilerShader* shader, uint64 key)
	{
		if (shader)
			vectorAppendUnique(shader->m_shaderStateCacheKeys, key);
	}

	void UnlinkKey(std::vector<uint64>& keys, uint64 key)
	{
		keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
	}
}

LatteFetchShader* LatteSHRC_GetActiveFetchShader()
{
	return s_activeFS;
}

LatteDecompilerShader* LatteSHRC_GetActiveVertexShader()
{
	return s_activeVS;
}

LatteDecompilerShader* LatteSHRC_GetActiveGeometryShader()
{
	return s_activeGS;
}

LatteDecompilerShader* LatteSHRC_GetActivePixelShader()
{
	return s_activePS;
}

void LatteSHRC_ResetCachedShaderHash()
{
	// forces the next draw to rehash its programs (their memory may have been rewritten)
	for (ProgramHashMemo* memo : { &s_memoVS, &s_memoGS, &s_memoPS })
	{
		memo->code = nullptr;
		memo->size = 0;
	}
}

LatteShaderPSInputTable* LatteSHRC_GetPSInputTable()
{
	return &s_psInputs;
}

void LatteSHRC_RemoveFromCacheByHash(uint64 shader_base_hash, uint64 shader_aux_hash, LatteConst::ShaderType type)
{
	LatteDecompilerShader* shader = nullptr;
	switch (type)
	{
	case LatteConst::ShaderType::Vertex:
		shader = LatteSHRC_FindVertexShader(shader_base_hash, shader_aux_hash);
		break;
	case LatteConst::ShaderType::Geometry:
		shader = LatteSHRC_FindGeometryShader(shader_base_hash, shader_aux_hash);
		break;
	case LatteConst::ShaderType::Pixel:
		shader = LatteSHRC_FindPixelShader(shader_base_hash, shader_aux_hash);
		break;
	default:
		break;
	}
	if (shader)
		Unregister(shader);
}

void LatteShader_free(LatteDecompilerShader* shader)
{
	Unregister(shader);
	delete shader->shader;
	shader->shader = nullptr;
	delete shader->strBuf_shaderSource;
	shader->strBuf_shaderSource = nullptr;
	delete shader;
}

void LatteShader_CreatePSInputTable(LatteShaderPSInputTable* psInputTable, uint32* contextRegisters)
{
	const uint32 control0 = contextRegisters[mmSPI_PS_IN_CONTROL_0];
	const bool positionEnabled = ((control0 >> 8) & 1) != 0;
	cemu_assert_debug(((control0 >> 9) & 1) == 0); // centroid position is not handled
	// input slot that receives the fragment position (gl_FragCoord), if any
	const uint32 positionSlot = positionEnabled ? ((control0 >> 10) & 0x1F) : 0xFFFFFFFF;
	// generated point coordinates (gl_PointCoord)
	const uint32 paramGen = (control0 >> 15) & 0xF;
	const uint32 paramGenGPR = (control0 >> 19) & 0x7F;

	uint64 key = 0;
	if (positionEnabled)
		key += (uint64)positionSlot + 1;
	if (paramGen != 0)
	{
		key += std::rotr<uint64>(paramGen, 7);
		key += std::rotr<uint64>(paramGenGPR, 3);
		psInputTable->paramGen = (uint8)paramGen;
		psInputTable->paramGenGPR = (uint8)paramGenGPR;
	}
	else
		psInputTable->paramGen = 0;

	uint32 inputCount = control0 & 0x3F;
	cemu_assert_debug(inputCount <= GPU7_PS_MAX_INPUTS);
	inputCount = std::min<uint32>(inputCount, GPU7_PS_MAX_INPUTS);
	for (uint32 slot = 0; slot < inputCount; slot++)
	{
		const uint32 inputControl = contextRegisters[mmSPI_PS_INPUT_CNTL_0 + slot];
		cemu_assert_debug(((inputControl >> 8) & 3) <= 1); // default value modes 2 and 3 are not handled
		cemu_assert_debug((inputControl & 0xFF) != 0xFF);
		key = std::rotl<uint64>(key + inputControl, 7);
		LatteShaderPSInputTable::psImport_t& import = psInputTable->import[slot];
		if (slot == positionSlot)
		{
			import.semanticId = LATTE_ANALYZER_IMPORT_INDEX_SPIPOSITION;
			import.isFlat = false;
			import.isNoPerspective = false;
			key += 0x33;
		}
		else
		{
			import.semanticId = inputControl & 0xFF;
			import.isFlat = ((inputControl >> 10) & 1) != 0;
			import.isNoPerspective = ((inputControl >> 12) & 1) != 0;
		}
	}
	psInputTable->key = key;
	psInputTable->count = (sint32)inputCount;
}

void LatteShader_UpdatePSInputs(uint32* contextRegisters)
{
	// vertex, geometry and pixel shaders all depend on the pixel shader inputs
	LatteShader_CreatePSInputTable(&s_psInputs, contextRegisters);
}

void LatteShader_CreateRendererShader(LatteDecompilerShader* shader, bool compileAsync)
{
	if (shader->hasError)
	{
		cemuLog_log(LogType::Force, "Unable to compile shader {:016x}", shader->baseHash);
		return;
	}
	RendererShader::ShaderType hostType = RendererShader::ShaderType::kVertex;
	GraphicPack2::GP_SHADER_TYPE packType = GraphicPack2::GP_SHADER_TYPE::VERTEX;
	if (shader->shaderType == LatteConst::ShaderType::Geometry)
	{
		hostType = RendererShader::ShaderType::kGeometry;
		packType = GraphicPack2::GP_SHADER_TYPE::GEOMETRY;
	}
	else if (shader->shaderType == LatteConst::ShaderType::Pixel)
	{
		hostType = RendererShader::ShaderType::kFragment;
		packType = GraphicPack2::GP_SHADER_TYPE::PIXEL;
	}
	// a graphic pack can replace the generated source
	const RendererAPI api = g_renderer->GetType();
	std::string source;
	if (const std::string* replacement = GraphicPack2::FindCustomShaderSource(shader->baseHash, shader->auxHash, packType, api == RendererAPI::Vulkan, api == RendererAPI::Metal))
	{
		source = *replacement;
		shader->isCustomShader = true;
	}
	else
		source = shader->strBuf_shaderSource->c_str();
	// vertex programs known to crash AMD drivers are skipped
	if (hostType == RendererShader::ShaderType::kVertex)
	{
		constexpr uint64 kBlockedVertexShaders[] = { 0x15bc7edf9de2ed30, 0x83a697d61a3b9202, 0x97bc44a5028381c6, 0x24838b84d15a1da1 };
		if (std::find(std::begin(kBlockedVertexShaders), std::end(kBlockedVertexShaders), shader->baseHash) != std::end(kBlockedVertexShaders))
		{
			cemuLog_logDebug(LogType::Force, "Skipped vertex shader {:016x} (AMD driver crash)", shader->baseHash);
			shader->shader = nullptr;
			shader->hasError = true;
			return;
		}
	}
	// the backend always compiles in the background, LatteShader_FinishCompilation waits for it
	shader->shader = g_renderer->shader_create(hostType, shader->baseHash, shader->auxHash, source, true, shader->isCustomShader);
	if (!shader->shader)
		shader->hasError = true;
	LatteShader_CleanupAfterCompile(shader);
}

void LatteShader_FinishCompilation(LatteDecompilerShader* shader)
{
	if (shader->hasError)
	{
		cemuLog_logDebug(LogType::Force, "Shader {:x} has an error, nothing to finish", shader->baseHash);
		return;
	}
	shader->shader->WaitForCompiled();
#ifdef ENABLE_OPENGL
	LatteShader_prepareSeparableUniforms(shader);
#endif
	LatteShader_CleanupAfterCompile(shader);
}

void LatteSHRC_RegisterShader(LatteDecompilerShader* shader, uint64 baseHash, uint64 auxHash)
{
	shader->baseHash = baseHash;
	shader->auxHash = auxHash;
	ShaderTable& table = TableFor(shader->shaderType);
	auto it = table.find(baseHash);
	if (it == table.end())
	{
		shader->next = nullptr;
		table.emplace(baseHash, shader);
		return;
	}
	// new variants go right behind the chain head
	shader->next = it->second->next;
	it->second->next = shader;
}

LatteDecompilerShader* LatteSHRC_FindVertexShader(uint64 baseHash, uint64 auxHash)
{
	return Lookup(s_vsTable, baseHash, auxHash);
}

LatteDecompilerShader* LatteSHRC_FindGeometryShader(uint64 baseHash, uint64 auxHash)
{
	return Lookup(s_gsTable, baseHash, auxHash);
}

LatteDecompilerShader* LatteSHRC_FindPixelShader(uint64 baseHash, uint64 auxHash)
{
	return Lookup(s_psTable, baseHash, auxHash);
}

LatteFetchShader* LatteSHRC_GetOrCreateFetchShader()
{
	return LatteFetchShader::FindByGPUState();
}

void LatteShader_CleanupAfterCompile(LatteDecompilerShader* shader)
{
	// the generated source is not needed once the backend has it
	delete shader->strBuf_shaderSource;
	shader->strBuf_shaderSource = nullptr;
}

namespace
{
	const char* StageSuffix(LatteConst::ShaderType type)
	{
		switch (type)
		{
		case LatteConst::ShaderType::Vertex: return "vs";
		case LatteConst::ShaderType::Geometry: return "gs";
		case LatteConst::ShaderType::Pixel: return "ps";
		default: return "";
		}
	}

	const char* RawDumpSuffix(uint32 type)
	{
		switch (type)
		{
		case SHADER_DUMP_TYPE_FETCH: return "fs";
		case SHADER_DUMP_TYPE_VERTEX: return "vs";
		case SHADER_DUMP_TYPE_GEOMETRY: return "gs";
		case SHADER_DUMP_TYPE_PIXEL: return "ps";
		case SHADER_DUMP_TYPE_COPY: return "copy";
		case SHADER_DUMP_TYPE_COMPUTE: return "compute";
		default: return "";
		}
	}
}

void LatteShader_DumpShader(uint64 baseHash, uint64 auxHash, LatteDecompilerShader* shader)
{
	if (!ActiveSettings::DumpShadersEnabled())
		return;
	FileStream* file = FileStream::createFile2(ActiveSettings::GetUserDataPath("dump/shaders/{:016x}_{:016x}_{}.txt", baseHash, auxHash, StageSuffix(shader->shaderType)));
	if (!file)
		return;
	if (shader->strBuf_shaderSource)
		file->writeData(shader->strBuf_shaderSource->c_str(), shader->strBuf_shaderSource->getLen());
	delete file;
}

void LatteShader_DumpRawShader(uint64 baseHash, uint64 auxHash, uint32 type, uint8* programCode, uint32 programLen)
{
	if (!ActiveSettings::DumpShadersEnabled())
		return;
	FileStream* file = FileStream::createFile2(ActiveSettings::GetUserDataPath("dump/shaders/{:016x}_{:016x}_{}.bin", baseHash, auxHash, RawDumpSuffix(type)));
	if (!file)
		return;
	file->writeData(programCode, programLen);
	delete file;
}

LatteDecompilerShader* LatteShader_CreateShaderFromDecompilerOutput(LatteDecompilerOutput_t& decompilerOutput, uint64 baseHash, bool calculateAuxHash, uint64 optionalAuxHash, uint32* contextRegister)
{
	LatteDecompilerShader* shader = decompilerOutput.shader;
	shader->baseHash = baseHash;
	switch (g_renderer->GetType())
	{
	case RendererAPI::OpenGL:
		shader->resourceMapping = decompilerOutput.resourceMappingGL;
		break;
	case RendererAPI::Vulkan:
		shader->resourceMapping = decompilerOutput.resourceMappingVK;
		break;
	default:
		break;
	}
	shader->textureUnitMask2 = decompilerOutput.textureUnitMask;
	shader->streamoutBufferWriteMask = decompilerOutput.streamoutBufferWriteMask;
	shader->hasStreamoutBufferWrite = decompilerOutput.streamoutBufferWriteMask.any();
	CopyUniformLayout(shader, decompilerOutput);
	if (!calculateAuxHash)
	{
		shader->auxHash = optionalAuxHash;
		return shader;
	}
	switch (decompilerOutput.shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		shader->auxHash = VertexShaderAuxHash(shader, contextRegister);
		break;
	case LatteConst::ShaderType::Geometry:
		shader->auxHash = GeometryShaderAuxHash(shader);
		break;
	case LatteConst::ShaderType::Pixel:
		shader->auxHash = PixelShaderAuxHash(shader, contextRegister);
		break;
	default:
		cemu_assert_debug(false);
		break;
	}
	return shader;
}

void LatteShader_GetDecompilerOptions(LatteDecompilerOptions& options, LatteConst::ShaderType shaderType, bool geometryShaderEnabled)
{
	options.usesGeometryShader = geometryShaderEnabled;
	options.useTFViaSSBO = g_renderer->UseTFViaSSBO();
	options.spirvInstrinsics.hasRoundingModeRTEFloat32 = false;
#ifdef ENABLE_VULKAN
	if (g_renderer->GetType() == RendererAPI::Vulkan)
		options.spirvInstrinsics.hasRoundingModeRTEFloat32 = VulkanRenderer::GetInstance()->HasSPRIVRoundingModeRTE32();
#endif
	options.strictMul = g_current_game_profile->GetAccurateShaderMul() != AccurateShaderMulOption::False;
}

void LatteSHRC_RemoveShaderStateCacheEntryByKey(uint64 key)
{
	auto it = s_stateCache.find(key);
	if (it == s_stateCache.end())
	{
		cemu_assert_suspicious();
		return;
	}
	StateEntry& entry = it->second;
	if (entry.fetchShader)
		UnlinkKey(entry.fetchShader->m_shaderStateCacheKeys, key);
	for (StageSet& set : entry.variants)
	{
		for (LatteDecompilerShader* shader : { set.vs, set.ps, set.gs })
		{
			if (shader)
				UnlinkKey(shader->m_shaderStateCacheKeys, key);
		}
	}
	g_shaderStateCacheSetAuxCount -= (int)entry.variants.size();
	s_stateCache.erase(it);
	g_shaderStateCacheSetCount--;
}

void LatteSHRC_CleanupShaderStateCache()
{
	constexpr uint32 kExpireAfterFrames = 300; // about five seconds at 60 fps
	constexpr sint32 kChecksPerCall = 30;
	constexpr sint32 kRemovalsPerCall = 8;
	if (s_stateCache.empty())
	{
		s_stateKeys.clear();
		s_stateSweepPos = 0;
		return;
	}
	auto dropKeyAtSweepPos = [&]()
	{
		s_stateKeys[s_stateSweepPos] = s_stateKeys.back();
		s_stateKeys.pop_back();
	};
	sint32 removed = 0;
	for (sint32 check = 0; check < kChecksPerCall && !s_stateKeys.empty(); check++)
	{
		if (s_stateSweepPos >= s_stateKeys.size())
			s_stateSweepPos = 0;
		const uint64 key = s_stateKeys[s_stateSweepPos];
		auto it = s_stateCache.find(key);
		if (it == s_stateCache.end())
		{
			// entry was removed together with one of its shaders
			dropKeyAtSweepPos();
			continue;
		}
		if ((uint32)(LatteGPUState.frameCounter - it->second.lastUsedFrame) < kExpireAfterFrames)
		{
			s_stateSweepPos++;
			continue;
		}
		if (removed >= kRemovalsPerCall)
			break;
		LatteSHRC_RemoveShaderStateCacheEntryByKey(key);
		dropKeyAtSweepPos();
		removed++;
	}
}

void LatteSHRC_UpdateActiveShaders()
{
	auto& ctx = LatteGPUState.contextNew;
	const auto gsMode = ctx.VGT_GS_MODE.get_MODE();
	cemu_assert_debug(ctx.VGT_GS_MODE.get_ES_PASSTHRU() == false); // not supported
	cemu_assert_debug(gsMode == Latte::LATTE_VGT_GS_MODE::E_MODE::OFF || gsMode == Latte::LATTE_VGT_GS_MODE::E_MODE::SCENARIO_G);
	const bool usesGeometryShader = gsMode != Latte::LATTE_VGT_GS_MODE::E_MODE::OFF;

	struct ProgramRange
	{
		uint32 addr = 0;
		uint32 size = 0;
		uint64 packed() const { return ((uint64)addr << 32) | size; }
	};
	auto programAt = [](uint32 startReg) { return ProgramRange{ Reg(startReg) << 8, Reg(startReg + 1) << 3 }; };
	const ProgramRange fetchProgram = programAt(mmSQ_PGM_START_FS);
	const ProgramRange pixelProgram = programAt(mmSQ_PGM_START_PS);
	ProgramRange vertexProgram, geometryProgram, copyProgram;
	if (usesGeometryShader)
	{
		// the vertex stage runs as export shader, the VS slot holds the GS copy program
		vertexProgram = programAt(mmSQ_PGM_START_ES);
		geometryProgram = programAt(mmSQ_PGM_START_GS);
		copyProgram = programAt(mmSQ_PGM_START_VS);
	}
	else
	{
		vertexProgram = programAt(mmSQ_PGM_START_VS);
		if (vertexProgram.addr == 0 || vertexProgram.size == 0)
		{
			debug_printf("No vertex shader program set\n");
			LatteGPUState.activeShaderHasError = true;
			return;
		}
	}

	// state the generated code depends on
	constexpr uint32 kVteMask = 0x3F; // viewport scale/offset enables
	constexpr uint32 kClipMask = 1u << 19; // DX_CLIP_SPACE_DEF
	constexpr uint32 kPrimitiveMask = 0x3F;
	constexpr uint32 kPsInControl0Mask = 0x3F | (1u << 8) | (0x1Fu << 10) | (0xFu << 15) | (0x7Fu << 19);
	constexpr uint32 kPsInControl1Mask = 0x1FFu << 8; // front face
	constexpr uint32 kInterpControl0Mask = 1u << 1; // point sprite coordinates
	DualStateHasher hasher;
	hasher.MixIn(fetchProgram.packed(), vertexProgram.packed());
	hasher.MixIn(pixelProgram.packed(), geometryProgram.packed());
	hasher.MixIn(((uint64)(ctx.PA_CL_VTE_CNTL.getRawValue() & kVteMask) << 32) | (ctx.PA_CL_CLIP_CNTL.getRawValue() & kClipMask),
		((uint64)(ctx.VGT_PRIMITIVE_TYPE.getRawValue() & kPrimitiveMask) << 32) | Reg(mmVGT_STRMOUT_EN));
	LatteShader_UpdatePSInputs(LatteGPUState.contextRegister);
	hasher.MixIn(copyProgram.packed(), s_psInputs.key);
	hasher.MixIn(Reg(mmSPI_PS_IN_CONTROL_0) & kPsInControl0Mask, Reg(mmSPI_PS_IN_CONTROL_1) & kPsInControl1Mask);
	hasher.MixIn(Reg(mmSPI_INTERP_CONTROL_0) & kInterpControl0Mask, 0);
	const uint64 stateKey = hasher.Finish();

	LatteGPUState.activeShaderHasError = false;
	StateEntry* entry = nullptr;
	auto it = s_stateCache.find(stateKey);
	if (it != s_stateCache.end())
	{
		entry = &it->second;
		entry->lastUsedFrame = LatteGPUState.frameCounter;
		const uint64 auxHash = CurrentAuxHash(entry->fetchShader, entry->primary.vs, entry->primary.ps);
		const StageSet* match = nullptr;
		if (entry->primary.auxHash == auxHash) [[likely]]
			match = &entry->primary;
		else
		{
			for (const StageSet& set : entry->variants)
			{
				if (set.auxHash == auxHash)
				{
					match = &set;
					break;
				}
			}
		}
		if (match)
		{
			Activate(entry->fetchShader, *match);
			LatteGPUState.activeShaderHasError = match->hasError;
			return;
		}
	}

	// look up (or translate) every stage
	LatteFetchShader* fetchShader = LatteSHRC_GetOrCreateFetchShader();
	s_activeFS = fetchShader;
	auto memPtr = [](uint32 addr) { return (uint8*)memory_getPointerFromPhysicalOffset(addr); };
	StageSet set;
	set.vs = AcquireVertexShader(memPtr(vertexProgram.addr), vertexProgram.size, usesGeometryShader, fetchShader);
	set.ps = AcquirePixelShader(memPtr(pixelProgram.addr), pixelProgram.size, usesGeometryShader);
	set.gs = AcquireGeometryShader(usesGeometryShader, memPtr(geometryProgram.addr), geometryProgram.size, memPtr(copyProgram.addr), copyProgram.size, set.vs);
	for (LatteDecompilerShader* shader : { set.vs, set.ps, set.gs })
	{
		if (shader && shader->hasError)
			set.hasError = true;
	}
	set.auxHash = CurrentAuxHash(fetchShader, set.vs, set.ps);

	if (!entry)
	{
		entry = &s_stateCache[stateKey];
		s_stateKeys.push_back(stateKey);
		entry->fetchShader = fetchShader;
		entry->primary = set;
		entry->lastUsedFrame = LatteGPUState.frameCounter;
		if (fetchShader)
			fetchShader->m_shaderStateCacheKeys.push_back(stateKey);
		g_shaderStateCacheSetCount++;
	}
	LinkKey(set.vs, stateKey);
	LinkKey(set.ps, stateKey);
	LinkKey(set.gs, stateKey);
	entry->variants.push_back(set);
	g_shaderStateCacheSetAuxCount++;
	Activate(entry->fetchShader, set);
}

sint32 LatteDecompiler_getTextureSamplerBaseIndex(LatteConst::ShaderType shaderType)
{
	switch (shaderType)
	{
	case LatteConst::ShaderType::Vertex:
		return Latte::SAMPLER_BASE_INDEX_VERTEX;
	case LatteConst::ShaderType::Pixel:
		return Latte::SAMPLER_BASE_INDEX_PIXEL;
	case LatteConst::ShaderType::Geometry:
		return Latte::SAMPLER_BASE_INDEX_GEOMETRY;
	default:
		cemu_assert_suspicious();
		return 0;
	}
}

void LatteSHRC_Init()
{
	cemu_assert_debug(s_vsTable.empty() && s_gsTable.empty() && s_psTable.empty());
	cemu_assert_debug(s_stateCache.empty() && s_stateKeys.empty());
}

void LatteSHRC_UnloadAll()
{
	for (ShaderTable* table : { &s_vsTable, &s_gsTable, &s_psTable })
	{
		while (!table->empty())
			LatteShader_free(table->begin()->second);
	}
	// freeing the shaders removed every state cache entry
	cemu_assert_debug(s_stateCache.empty());
	s_stateKeys.clear();
	s_stateSweepPos = 0;
	// nothing may point at the freed shaders (the geometry hash reads the active vertex shader)
	s_activeFS = nullptr;
	s_activeVS = nullptr;
	s_activeGS = nullptr;
	s_activePS = nullptr;
}
