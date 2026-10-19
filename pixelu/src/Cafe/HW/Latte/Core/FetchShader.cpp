// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInstructions.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/ISA/LatteInstructions.h"
#include "HW/Latte/Renderer/Renderer.h"
#include "util/containers/LookupTableL3.h"
#include "util/helpers/fspinlock.h"
#ifdef ENABLE_METAL
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#endif
#include <openssl/sha.h>
#include <openssl/evp.h>

void LatteSHRC_RemoveShaderStateCacheEntryByKey(uint64 key);

std::unordered_map<LatteFetchShader::CacheHash, LatteFetchShader*> LatteFetchShader::s_fetchShaderByHash;

namespace
{
	using VTX = LatteClauseInstruction_VTX;

	// bytes read by one fetch of the given vertex data format, 0 if unsupported
	uint32 FetchFormatSize(Latte::E_HWFMT fmt)
	{
		using F = Latte::E_HWFMT;
		switch (fmt)
		{
		case F::HWFMT_8: return 1;
		case F::HWFMT_8_8:
		case F::HWFMT_16:
		case F::HWFMT_16_FLOAT: return 2;
		case F::HWFMT_8_8_8_8:
		case F::HWFMT_16_16:
		case F::HWFMT_16_16_FLOAT:
		case F::HWFMT_32:
		case F::HWFMT_32_FLOAT:
		case F::HWFMT_2_10_10_10: return 4;
		case F::HWFMT_16_16_16_16:
		case F::HWFMT_16_16_16_16_FLOAT:
		case F::HWFMT_32_32:
		case F::HWFMT_32_32_FLOAT: return 8;
		case F::HWFMT_32_32_32:
		case F::HWFMT_32_32_32_FLOAT: return 12;
		case F::HWFMT_32_32_32_32:
		case F::HWFMT_32_32_32_32_FLOAT: return 16;
		default:
			cemu_assert_unimplemented();
			return 0;
		}
	}

	// registry lock for s_fetchShaderByHash (lookups are done without it)
	FSpinlock s_registryLock;

	// per program address: the fetch shader that was last decoded from there
	struct AddressSlot
	{
		LatteFetchShader* fetchShader;
		uint32 programSize;
		uint32 frame; // frame counter of the last validation
	};
	LookupTableL3<8, 8, 8, AddressSlot*> s_byAddress;

	// add (or extend) the group of a buffer slot and append one decoded fetch to it
	void AddFetch(LatteFetchShader* fs, uint32* contextRegister, const VTX* fetch)
	{
		const uint32 bufferId = fetch->getField_BUFFER_ID();
		const uint32 slot = bufferId - 0xA0; // fetch constants for vertex buffers start at 0xA0
		cemu_assert_debug(bufferId >= 0xA0 && bufferId < 0xB0);
		const Latte::E_HWFMT format = fetch->getField_DATA_FORMAT();
		const uint32 offset = fetch->getField_OFFSET();
		const uint32 size = FetchFormatSize(format);
		cemu_assert(size > 0);

		const bool valid = LatteFetchShader::isValidBufferIndex(slot);
		auto& groups = valid ? fs->bufferGroups : fs->bufferGroupsInvalid;
		LatteParsedFetchShaderBufferGroup* group = nullptr;
		for (auto& g : groups)
		{
			if (g.attributeBufferIndex == slot)
			{
				group = &g;
				break;
			}
		}
		if (!group)
		{
			group = &groups.emplace_back();
			group->attributeBufferIndex = (uint8)slot;
			group->minOffset = offset;
			group->totalAttribRangeSize = offset;
			// slots beyond the mask width wrap like a hardware shift
			fs->attributeBufferMask |= (1u << (slot & 31));
		}

		cemu_assert(group->attribCount < 127);
		const sint32 n = group->attribCount;
		group->attribCount = (sint8)(n + 1);
		group->attrib = (LatteParsedFetchShaderAttribute*)realloc(group->attrib, sizeof(LatteParsedFetchShaderAttribute) * (n + 1));
		LatteParsedFetchShaderAttribute& a = group->attrib[n];
		a.attributeBufferIndex = (uint8)slot;
		a.semanticId = (uint8)fetch->getFieldSEM_SEMANTIC_ID();
		a.format = format;
		a.fetchType = fetch->getField_FETCH_TYPE();
		a.nfa = (uint8)fetch->getField_NUM_FORMAT_ALL();
		a.isSigned = fetch->getField_FORMAT_COMP_ALL() == VTX::FORMAT_COMP::COMP_SIGNED;
		a.endianSwap = fetch->getField_ENDIAN_SWAP();
		for (uint32 c = 0; c < 4; c++)
			a.ds[c] = (uint8)fetch->getField_DST_SEL(c);
		a.offset = offset;
		group->minOffset = std::min(group->minOffset, offset);
		group->totalAttribRangeSize = std::max(group->totalAttribRangeSize, offset + size);

		// the fetch index source selects the divisor: X = vertex id, W = instance id,
		// Y/Z = instance id divided by VGT_INSTANCE_STEP_RATE_0/1
		switch (fetch->getField_SRC_SEL_X())
		{
		case VTX::SRC_SEL::SEL_X:
			cemu_assert_debug(a.fetchType != LatteConst::VertexFetchType2::INSTANCE_DATA);
			a.aluDivisor = -1;
			break;
		case VTX::SRC_SEL::SEL_W:
			cemu_assert_debug(a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA);
			a.aluDivisor = 1;
			break;
		case VTX::SRC_SEL::SEL_Y:
			a.aluDivisor = (sint32)contextRegister[Latte::REGADDR::VGT_INSTANCE_STEP_RATE_0];
			cemu_assert_debug(a.aluDivisor > 0);
			break;
		case VTX::SRC_SEL::SEL_Z:
			a.aluDivisor = (sint32)contextRegister[Latte::REGADDR::VGT_INSTANCE_STEP_RATE_1];
			cemu_assert_debug(a.aluDivisor > 0);
			break;
		}
	}

	// decode a program that starts with a CF program (how GX2 lays out fetch shaders)
	void DecodeWithCF(LatteFetchShader* fs, uint32* contextRegister, std::span<uint8> program)
	{
		const size_t cfCount = program.size() / sizeof(LatteCFInstruction);
		const LatteCFInstruction* cf = (const LatteCFInstruction*)program.data();
		for (size_t i = 0; i < cfCount; i++, cf++)
		{
			const auto op = cf->getField_Opcode();
			if (op == LatteCFInstruction::INST_RETURN)
				return;
			if (op == LatteCFInstruction::INST_VTX_TC)
			{
				const auto* clause = cf->getParserIfOpcodeMatch<LatteCFInstruction_DEFAULT>();
				cemu_assert_debug(clause->getField_COND() == LatteCFInstruction::CF_COND::CF_COND_ACTIVE);
				// clamp the clause to the program (out of range clauses are ignored)
				const uint64 start = clause->getField_ADDR();
				uint64 fetchCount = clause->getField_COUNT();
				if (start > program.size())
					fetchCount = 0;
				else
					fetchCount = std::min<uint64>(fetchCount, (program.size() - start) / sizeof(VTX));
				const VTX* fetch = (const VTX*)(program.data() + start);
				for (uint64 f = 0; f < fetchCount; f++)
				{
					// only semantic fetches are expected in a fetch shader
					if (fetch[f].getField_VTX_INST() == VTX::VTX_INST::_VTX_INST_SEMANTIC)
						AddFetch(fs, contextRegister, fetch + f);
					else
						cemu_assert_debug(false);
				}
			}
			else
				cemu_assert_debug(false);
			if (cf->getField_END_OF_PROGRAM())
				return;
		}
	}

	// Older PixelU/Cemu shader caches hold fetch shaders that were generated without the
	// CF part, just a run of 16 byte fetch instructions. Those are scanned dword by dword.
	void DecodeBareFetches(LatteFetchShader* fs, uint32* contextRegister, uint32* code, uint32 dwordCount)
	{
		uint32 pos = 0;
		while (pos < dwordCount)
		{
			const uint32 op = code[pos] & 0x1F;
			if (op == VTX_INST_SEMANTIC)
			{
				if (pos + 4 <= dwordCount)
					AddFetch(fs, contextRegister, (const VTX*)(code + pos));
				pos += 4;
			}
			else if (op == VTX_INST_MEM)
				pos += 4;
			else
				pos += 1;
		}
	}

	// true if the first 8 bytes look like a CF instruction of a GX2 fetch shader
	// (an early CALL_FS-style clause header: small ADDR, VTX_TC opcode with any count)
	bool StartsWithCFProgram(const uint32* code)
	{
		return (code[0] & 1) == 0 && code[0] <= 0x30 && (code[1] & ~((3u << 10) | (1u << 19))) == 0x01800000;
	}

	// hash of the decoded layout, used to tell fetch shaders apart in shader lookups
	void ComputeLayoutKey(LatteFetchShader* fs)
	{
		uint64 key = 0;
		auto mix = [&key](uint64 v, int rot) { key = std::rotl<uint64>(key + v, rot); };
		const bool metal = g_renderer->GetType() == RendererAPI::Metal;
		for (auto& group : fs->bufferGroups)
		{
			for (sint32 i = 0; i < group.attribCount; i++)
			{
				const auto& a = group.attrib[i];
				mix((uint64)a.endianSwap, 3);
				mix((uint64)a.nfa, 3);
				mix(a.isSigned ? 1 : 0, 1);
				mix((uint64)a.format, 7);
				mix((uint64)a.fetchType, 8);
				mix(a.ds[0], 2);
				mix(a.ds[1], 2);
				mix(a.ds[2], 2);
				mix(a.ds[3], 2);
				mix((uint64)(a.aluDivisor + 1), 2);
				mix(a.attributeBufferIndex, 8);
				mix(a.semanticId, 8);
#ifdef ENABLE_METAL
				if (metal)
					mix(a.offset, 7);
				else
#endif
					mix(a.offset & 3, 2);
			}
		}
#ifdef ENABLE_METAL
		if (metal)
		{
			for (auto& group : fs->bufferGroups)
				mix(group.attributeBufferIndex, 5);
		}
#endif
		(void)metal;
		fs->key = key;
	}

	void FinishDecode(LatteFetchShader* fs, uint32* contextRegister)
	{
		ComputeLayoutKey(fs);
		fs->CalculateFetchShaderVkHash();
#ifdef ENABLE_METAL
		fs->CheckIfVerticesNeedManualFetchMtl(contextRegister);
#endif
	}

	uint32 CurrentProgramAddr24()
	{
		return LatteGPUState.contextRegister[mmSQ_PGM_START_FS + 0];
	}

	void* CurrentProgramPtr()
	{
		return memory_getPointerFromPhysicalOffset(CurrentProgramAddr24() << 8);
	}

	uint32 CurrentProgramSize()
	{
		return LatteGPUState.contextRegister[mmSQ_PGM_START_FS + 1] << 3;
	}

	LatteFetchShader* GetOrCreateForCurrentProgram(LatteFetchShader::CacheHash hash)
	{
		LatteFetchShader* fs = LatteFetchShader::FindInCacheByHash(hash);
		if (!fs)
		{
			fs = LatteShaderRecompiler_createFetchShader(hash, LatteGPUState.contextNew.GetRawView(), (uint32*)CurrentProgramPtr(), CurrentProgramSize());
			cemu_assert(fs);
		}
		return fs;
	}
}

uint32 LatteParsedFetchShaderBufferGroup::getCurrentBufferStride(uint32* contextRegister) const
{
	// each vertex buffer has 7 registers; the stride sits in bits 11-26 of the third
	return (contextRegister[mmSQ_VTX_ATTRIBUTE_BLOCK_START + attributeBufferIndex * 7 + 2] >> 11) & 0xFFFF;
}

void LatteFetchShader::CalculateFetchShaderVkHash()
{
	// SHA-1 over (index in group, offset) of every attribute, folded to 64 bits.
	// This value is part of Vulkan pipeline cache keys, so the formula must not change.
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	EVP_DigestInit(ctx, EVP_sha1());
	for (auto& group : bufferGroups)
	{
		for (sint32 i = 0; i < group.attribCount; i++)
		{
			const sint32 index = i;
			const uint32 offset = group.attrib[i].offset;
			EVP_DigestUpdate(ctx, &index, sizeof(index));
			EVP_DigestUpdate(ctx, &offset, sizeof(offset));
		}
	}
	uint8 digest[SHA_DIGEST_LENGTH];
	EVP_DigestFinal_ex(ctx, digest, nullptr);
	EVP_MD_CTX_free(ctx);
	uint64 a, b;
	uint32 c;
	memcpy(&a, digest, 8);
	memcpy(&b, digest + 8, 8);
	memcpy(&c, digest + 16, 4);
	vkPipelineHashFragment = a + b + (uint64)c;
}

#ifdef ENABLE_METAL
void LatteFetchShader::CheckIfVerticesNeedManualFetchMtl(uint32* contextRegister)
{
	// Metal needs 4 byte aligned strides and attributes that end inside the stride
	for (auto& group : bufferGroups)
	{
		const uint32 stride = group.getCurrentBufferStride(contextRegister);
		if ((stride & 3) != 0)
			mtlFetchVertexManually = true;
		for (sint32 i = 0; i < group.attribCount; i++)
		{
			const auto& a = group.attrib[i];
			if (a.offset + GetMtlVertexFormatSize(a.format) > stride)
				mtlFetchVertexManually = true;
		}
	}
}
#endif

// Decodes a fetch shader program into a LatteFetchShader and registers it under fsHash.
// Safe to call from several threads for the same program: only one object gets
// registered and the others are deleted in favour of it.
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash fsHash, uint32* contextRegister, uint32* fsProgramCode, uint32 fsProgramSize)
{
	LatteFetchShader* fs = new LatteFetchShader();
	fs->m_cacheHash = fsHash;
	if ((fsProgramSize & 0xF) != 0)
		debugBreakpoint();
	fs->bufferGroups.reserve(16);

	if (fsProgramSize == 0)
	{
		// some games bind an empty fetch shader; it is not registered
		FinishDecode(fs, contextRegister);
		return fs;
	}

	if (fsProgramSize >= 8 && StartsWithCFProgram(fsProgramCode))
		DecodeWithCF(fs, contextRegister, { (uint8*)fsProgramCode, fsProgramSize });
	else
		DecodeBareFetches(fs, contextRegister, fsProgramCode, fsProgramSize / 4);

	fs->bufferGroups.shrink_to_fit();
	cemu_assert(fs->bufferGroups.size() <= Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS);
	for (auto& group : fs->bufferGroups)
	{
		bool perVertex = false, perInstance = false;
		for (sint32 i = 0; i < group.attribCount; i++)
		{
			perVertex |= group.attrib[i].fetchType == LatteConst::VERTEX_DATA;
			perInstance |= group.attrib[i].fetchType == LatteConst::INSTANCE_DATA;
		}
		group.hasVtxIndexAccess = perVertex;
		group.hasInstanceIndexAccess = perInstance;
	}
	FinishDecode(fs, contextRegister);

	if (LatteFetchShader* existing = fs->RegisterInCache(fsHash))
	{
		delete fs;
		return existing;
	}
	fs->m_isRegistered = true;
	return fs;
}

LatteFetchShader::~LatteFetchShader()
{
	UnregisterInCache();
	// each removal takes the key off m_shaderStateCacheKeys
	while (!m_shaderStateCacheKeys.empty())
		LatteSHRC_RemoveShaderStateCacheEntryByKey(m_shaderStateCacheKeys.back());
}

LatteFetchShader::CacheHash LatteFetchShader::CalculateCacheHash(void* programCode, uint32 programSize)
{
	// two rolling 64 bit accumulators over the program dwords (sum + rotate left, xor + rotate right).
	// The result names shaders in shader caches and graphic packs, so it must stay as is.
	const uint32* code = (const uint32*)programCode;
	uint64 sum = 0, mix = 0;
	for (uint32 i = 0, n = programSize / 4; i < n; i++)
	{
		sum = std::rotl<uint64>(sum + code[i], 3);
		mix = std::rotr<uint64>(mix ^ code[i], 7);
	}
	return sum + mix;
}

LatteFetchShader* LatteFetchShader::FindInCacheByHash(LatteFetchShader::CacheHash fsHash)
{
	// lock free: registration only happens on the thread that calls this, or during cache loading
	auto it = s_fetchShaderByHash.find(fsHash);
	return it == s_fetchShaderByHash.end() ? nullptr : it->second;
}

LatteFetchShader* LatteFetchShader::FindByGPUState()
{
	const uint32 addr24 = CurrentProgramAddr24();
	cemu_assert_debug(addr24 < 0x1000000);
	const uint32 frame = LatteGPUState.frameCounter;
	AddressSlot* slot = s_byAddress.lookup(addr24);
	if (!slot)
	{
		LatteFetchShader* fs = GetOrCreateForCurrentProgram(CalculateCacheHash(CurrentProgramPtr(), CurrentProgramSize()));
		slot = new AddressSlot{ fs, CurrentProgramSize(), frame };
		s_byAddress.store(addr24, slot);
		return fs;
	}
	const uint32 size = CurrentProgramSize();
	// within a frame the program at an address is assumed not to change
	if (slot->programSize == size && slot->frame == frame)
		return slot->fetchShader;
	const CacheHash hash = CalculateCacheHash(CurrentProgramPtr(), size);
	if (slot->programSize != size || slot->fetchShader->m_cacheHash != hash)
	{
		slot->fetchShader = GetOrCreateForCurrentProgram(hash);
		slot->programSize = size;
	}
	slot->frame = frame;
	return slot->fetchShader;
}

LatteFetchShader* LatteFetchShader::RegisterInCache(CacheHash fsHash)
{
	s_registryLock.lock();
	auto [it, inserted] = s_fetchShaderByHash.try_emplace(fsHash, this);
	LatteFetchShader* existing = inserted ? nullptr : it->second;
	s_registryLock.unlock();
	return existing;
}

void LatteFetchShader::UnregisterInCache()
{
	if (!m_isRegistered)
		return;
	s_registryLock.lock();
	auto it = s_fetchShaderByHash.find(m_cacheHash);
	if (it != s_fetchShaderByHash.end() && it->second == this)
		s_fetchShaderByHash.erase(it);
	s_registryLock.unlock();
	m_isRegistered = false;
}
