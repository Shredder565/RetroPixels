// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"

// A Latte fetch shader is a small program of vertex fetch instructions that the
// vertex shader calls to load its inputs. PixelU never runs it; it is decoded
// into the attribute layout below, which the shader generators and the renderers
// turn into host vertex input state.

// one vertex fetch (one shader input)
struct LatteParsedFetchShaderAttribute
{
	uint8								attributeBufferIndex; // vertex buffer slot (0-15 valid)
	uint8								semanticId; // input location seen by the vertex shader
	Latte::E_HWFMT						format;
	LatteConst::VertexFetchType2		fetchType; // per vertex or per instance
	uint8								nfa; // number format (norm/int/scaled)
	uint8								isSigned;
	LatteConst::VertexFetchEndianMode	endianSwap;
	uint8								ds[4]; // destination swizzle per output component
	sint32								aluDivisor; // -1 = per vertex, otherwise instance step rate
	uint32								offset; // byte offset inside the vertex
};

// all fetches that read from the same vertex buffer
struct LatteParsedFetchShaderBufferGroup
{
	uint8 attributeBufferIndex{};
	sint8 attribCount{};
	bool hasVtxIndexAccess : 1;
	bool hasInstanceIndexAccess : 1;
	uint32 minOffset{}; // smallest attribute offset
	uint32 totalAttribRangeSize{}; // largest offset + attribute size
	LatteParsedFetchShaderAttribute* attrib{}; // attribCount entries (malloc'd)

	// stride of this group's buffer as currently set in the SQ_VTX_ATTRIBUTE block
	uint32 getCurrentBufferStride(uint32* contextRegister) const;
};

struct LatteFetchShader
{
	using CacheHash = uint64;

	~LatteFetchShader();

	std::vector<LatteParsedFetchShaderBufferGroup> bufferGroups;
	// fetches from buffer ids outside 0-15: no data is fetched, but their swizzles still shape the shader
	std::vector<LatteParsedFetchShaderBufferGroup> bufferGroupsInvalid;

	uint64 key{}; // compact hash of the decoded attribute layout
	uint32 attributeBufferMask{}; // bit per referenced buffer slot

	// Vulkan: hash of the parts of the layout that are baked into a graphics pipeline
	uint64 vkPipelineHashFragment{};

	CacheHash m_cacheHash{}; // hash of the program code
	bool m_isRegistered{}; // true once this object owns its m_cacheHash entry

	void CalculateFetchShaderVkHash();


	uint64 getVkPipelineHashFragment() const { return vkPipelineHashFragment; };

	static bool isValidBufferIndex(const uint32 index) { return index < 0x10; };

	// shader state cache entries that reference this fetch shader
	std::vector<uint64> m_shaderStateCacheKeys;

	// registry of decoded fetch shaders by program hash
	LatteFetchShader* RegisterInCache(CacheHash fsHash); // returns the already registered object, or null if this one got registered
	void UnregisterInCache();
	static CacheHash CalculateCacheHash(void* programCode, uint32 programSize);
	static LatteFetchShader* FindInCacheByHash(CacheHash fsHash);
	static LatteFetchShader* FindByGPUState();

	static std::unordered_map<CacheHash, LatteFetchShader*> s_fetchShaderByHash;
};

LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash fsHash, uint32* contextRegister, uint32* fsProgramCode, uint32 fsProgramSize);
