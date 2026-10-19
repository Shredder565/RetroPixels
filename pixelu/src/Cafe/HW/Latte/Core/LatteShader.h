// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"

// shader manager (LatteShader.cpp)
void LatteSHRC_Init();
void LatteSHRC_UnloadAll();

void LatteSHRC_ResetCachedShaderHash();
LatteFetchShader* LatteSHRC_GetOrCreateFetchShader();

// picks (and if needed translates) the shaders for the current GPU state
void LatteSHRC_UpdateActiveShaders();
// expires shader state cache entries that were not used for a while, call once per frame
void LatteSHRC_CleanupShaderStateCache();

struct LatteFetchShader* LatteSHRC_GetActiveFetchShader();
LatteDecompilerShader* LatteSHRC_GetActiveVertexShader();
LatteDecompilerShader* LatteSHRC_GetActiveGeometryShader();
LatteDecompilerShader* LatteSHRC_GetActivePixelShader();

LatteDecompilerShader* LatteSHRC_FindVertexShader(uint64 baseHash, uint64 auxHash);
LatteDecompilerShader* LatteSHRC_FindGeometryShader(uint64 baseHash, uint64 auxHash);
LatteDecompilerShader* LatteSHRC_FindPixelShader(uint64 baseHash, uint64 auxHash);

#define GPU7_PS_MAX_INPUTS	32

// pixel shader input slots and the vertex shader output each one reads
struct LatteShaderPSInputTable
{
	struct psImport_t
	{
		uint32 semanticId;
		bool isFlat;
		bool isNoPerspective;
	};
	psImport_t import[GPU7_PS_MAX_INPUTS]{}; // indexed by input GPR
	sint32 count;
	uint64 key; // hash of the input setup
	uint8 paramGen; // point coordinate generation mask
	uint8 paramGenGPR;

	// semantic id of the vertex shader output at the given index; not every output has a reader
	static sint32 getVertexShaderOutParamSemanticId(uint32* contextRegisters, sint32 index)
	{
		cemu_assert_debug(index >= 0 && index < 32);
		const uint32 packed = contextRegisters[mmSPI_VS_OUT_ID_0 + index / 4];
		return (sint32)((packed >> ((index % 4) * 8)) & 0xFF);
	}

	// any slot reads the semantic (no range limit, unlike the lookups below)
	bool hasPSImportForSemanticId(sint32 semanticId)
	{
		for (sint32 slot = 0; slot < count; slot++)
		{
			if ((sint32)import[slot].semanticId == semanticId)
				return true;
		}
		return false;
	}

	psImport_t* getPSImportBySemanticId(sint32 semanticId)
	{
		const sint32 location = getPSImportLocationBySemanticId(semanticId);
		return location >= 0 ? import + location : nullptr;
	}

	// input slot that reads the given semantic, -1 if none
	sint32 getPSImportLocationBySemanticId(sint32 semanticId)
	{
		if (semanticId > LATTE_ANALYZER_IMPORT_INDEX_PARAM_MAX)
			return -1;
		for (sint32 slot = 0; slot < count; slot++)
		{
			if ((sint32)import[slot].semanticId == semanticId)
				return slot;
		}
		return -1;
	}
};

void LatteShader_CreatePSInputTable(LatteShaderPSInputTable* psInputTable, uint32* contextRegisters);
void LatteShader_UpdatePSInputs(uint32* contextRegisters);
LatteShaderPSInputTable* LatteSHRC_GetPSInputTable();

void LatteShader_free(LatteDecompilerShader* shader);
void LatteSHRC_RemoveFromCacheByHash(uint64 shader_base_hash, uint64 shader_aux_hash, LatteConst::ShaderType type);

// base hashes computed by the last LatteSHRC_UpdateActiveShaders
extern uint64 _shaderBaseHash_fs;
extern uint64 _shaderBaseHash_vs;
extern uint64 _shaderBaseHash_gs;
extern uint64 _shaderBaseHash_ps;

void LatteShader_GetDecompilerOptions(struct LatteDecompilerOptions& options, LatteConst::ShaderType shaderType, bool geometryShaderEnabled);
LatteDecompilerShader* LatteShader_CreateShaderFromDecompilerOutput(LatteDecompilerOutput_t& decompilerOutput, uint64 baseHash, bool calculateAuxHash, uint64 optionalAuxHash, uint32* contextRegister);

void LatteShader_CreateRendererShader(LatteDecompilerShader* shader, bool compileAsync);
void LatteShader_FinishCompilation(LatteDecompilerShader* shader);

void LatteShader_prepareSeparableUniforms(LatteDecompilerShader* shader);

void LatteSHRC_RegisterShader(LatteDecompilerShader* shader, uint64 baseHash, uint64 auxHash);

void LatteShader_CleanupAfterCompile(LatteDecompilerShader* shader);

// kinds of raw program dumps
#define SHADER_DUMP_TYPE_FETCH		0
#define SHADER_DUMP_TYPE_VERTEX		1
#define SHADER_DUMP_TYPE_GEOMETRY	2
#define SHADER_DUMP_TYPE_PIXEL		3
#define SHADER_DUMP_TYPE_COPY		4
#define SHADER_DUMP_TYPE_COMPUTE	5

void LatteShader_DumpShader(uint64 baseHash, uint64 auxHash, LatteDecompilerShader* shader);
void LatteShader_DumpRawShader(uint64 baseHash, uint64 auxHash, uint32 type, uint8* programCode, uint32 programLen);

// transferable shader cache (LatteShaderCache.cpp)
void LatteShaderCache_Load();
void LatteShaderCache_Close();

void LatteShaderCache_writeSeparableVertexShader(uint64 shaderBaseHash, uint64 shaderAuxHash, uint8* fetchShader, uint32 fetchShaderSize, uint8* vertexShader, uint32 vertexShaderSize, uint32* contextRegisters, bool usesGeometryShader);
void LatteShaderCache_writeSeparableGeometryShader(uint64 shaderBaseHash, uint64 shaderAuxHash, uint8* geometryShader, uint32 geometryShaderSize, uint8* gsCopyShader, uint32 gsCopyShaderSize, uint32* contextRegisters, uint32* hleSpecialState, uint32 vsRingParameterCount);
void LatteShaderCache_writeSeparablePixelShader(uint64 shaderBaseHash, uint64 shaderAuxHash, uint8* pixelShader, uint32 pixelShaderSize, uint32* contextRegisters, bool usesGeometryShader);

// first sampler slot of a shader stage
sint32 LatteDecompiler_getTextureSamplerBaseIndex(LatteConst::ShaderType shaderType);
