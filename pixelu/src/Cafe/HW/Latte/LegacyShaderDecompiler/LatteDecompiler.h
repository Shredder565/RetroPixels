// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Renderer/RendererShader.h"
#include <boost/container/static_vector.hpp>

// Public interface of the legacy Latte shader decompiler: it turns a Latte
// vertex/geometry/pixel program plus the current register state into GLSL
// (OpenGL/Vulkan) or MSL source and describes the resources the result uses.

namespace LatteDecompiler
{
	// placeholder kept for source compatibility; all members share one value
	enum class DataType
	{
		UNDEFINED = 0,
		U32 = 0,
		S32 = 0,
		FLOAT = 0
	};
};

// a constant (register or buffer entry) that was packed into the compact uniform array
typedef struct
{
	bool	isRegister; // true: constant file register, false: constant buffer entry
	uint8	kcacheBankId; // buffer slot when isRegister is false
	uint32	index; // vec4 index in the source
	uint32	mappedIndex; // vec4 index in the packed array
}LatteDecompilerRemappedUniformEntry_t;

// pre-scaled (byte offset) form of a remapped register entry
typedef struct
{
	uint32	indexOffset;
	uint32	mappedIndexOffset;
}LatteFastAccessRemappedUniformEntry_register_t;

// pre-scaled (byte offset) form of a remapped buffer entry
typedef struct
{
	uint16	indexOffset;
	uint16	mappedIndexOffset;
}LatteFastAccessRemappedUniformEntry_buffer_t;

typedef struct
{
	uint32 texUnit;
	sint32 uniformLocation;
	float currentValue[2];
}LatteUniformTextureScaleEntry_t;

// binding slots of everything a generated shader references, per backend
struct LatteDecompilerShaderResourceMapping
{
	static constexpr sint8 UNUSED_BINDING = -1;

	LatteDecompilerShaderResourceMapping()
	{
		for (auto& v : textureUnitToBindingPoint) v = UNUSED_BINDING;
		for (auto& v : relBindingPointToRelTextureUnit) v = UNUSED_BINDING;
		for (auto& v : uniformBuffersBindingPoint) v = UNUSED_BINDING;
		for (auto& v : attributeMapping) v = UNUSED_BINDING;
	}
	// textures
	sint8 textureUnitToBindingPoint[LATTE_NUM_MAX_TEX_UNITS]; // absolute binding per texture unit (OpenGL keeps units sparse)
	sint8 relBindingPointToRelTextureUnit[LATTE_NUM_MAX_TEX_UNITS]; // Vulkan: (binding - base) -> texture unit
	sint8 textureUnitBaseBindingPoint{UNUSED_BINDING};
	sint8 textureUnitCount{0};
	// uniforms
	sint8 uniformVarsBufferBindingPoint{UNUSED_BINDING}; // block holding uniform registers / remapped constants / helper values
	sint8 uniformBuffersBindingPoint[LATTE_NUM_MAX_UNIFORM_BUFFERS];
	// storage buffer used when transform feedback is emulated with an SSBO
	sint8 tfStorageBindingPoint{UNUSED_BINDING};
	// vertex attributes, indexed by semantic id
	sint8 attributeMapping[LATTE_NUM_MAX_ATTRIBUTE_LOCATIONS];
	// Vulkan descriptor set
	sint8 setIndex{};

	sint32 getTextureCount()
	{
		return textureUnitCount;
	}

	sint32 getRelativeTextureUnitFromRelativeBindingPoint(sint8 relativeBindingPoint)
	{
		cemu_assert_debug(relativeBindingPoint >= 0 && relativeBindingPoint < LATTE_NUM_MAX_TEX_UNITS);
		return relBindingPointToRelTextureUnit[relativeBindingPoint];
	}

	// -1 when the shader samples no texture
	sint32 getTextureBaseBindingPoint()
	{
		return textureUnitBaseBindingPoint;
	}

	// lowest and highest binding used by any uniform block; false if there is none
	bool getUniformBufferBindingRange(sint32& minBinding, sint32& maxBinding)
	{
		bool found = false;
		sint32 lo = 0, hi = 0;
		auto consider = [&](sint8 binding)
		{
			if (binding < 0)
				return;
			if (!found)
			{
				lo = hi = binding;
				found = true;
				return;
			}
			lo = std::min(lo, (sint32)binding);
			hi = std::max(hi, (sint32)binding);
		};
		consider(uniformVarsBufferBindingPoint);
		for (sint8 b : uniformBuffersBindingPoint)
			consider(b);
		if (!found)
			return false;
		minBinding = lo;
		maxBinding = hi;
		return true;
	}

	sint32 getTFStorageBufferBindingPoint()
	{
		return tfStorageBindingPoint;
	}

	bool hasUniformBuffers()
	{
		for (sint8 b : uniformBuffersBindingPoint)
		{
			if (b >= 0)
				return true;
		}
		return false;
	}

	sint32 getAttribHostShaderIndex(uint32 semanticId)
	{
		cemu_assert_debug(semanticId < 0x100);
		return attributeMapping[semanticId];
	}
};

// result object of a decompilation; owned by the shader cache afterwards
struct LatteDecompilerShader
{
	LatteDecompilerShader(LatteConst::ShaderType shaderType) : shaderType(shaderType) {}

	LatteDecompilerShader* next{nullptr};
	LatteConst::ShaderType shaderType;
	uint64 baseHash{0};
	uint64 auxHash{0};
	// vertex shaders: the fetch shader this was generated against
	struct LatteFetchShader* compatibleFetchShader{};
	// set when the program could not be translated
	bool hasError{false};
	// uniform buffers the shader reads and how many bytes of each
	struct QuickBufferEntry
	{
		uint32 index : 8;
		uint32 size : 24;
	};
	boost::container::static_vector<QuickBufferEntry, LATTE_NUM_MAX_UNIFORM_BUFFERS> list_quickBufferList;
	uint8 textureUnitList[LATTE_NUM_MAX_TEX_UNITS];
	uint8 textureUnitListCount{ 0 };
	// texture state the code was specialised for
	Latte::E_DIM textureUnitDim[LATTE_NUM_MAX_TEX_UNITS]{};
	bool textureIsIntegerFormat[LATTE_NUM_MAX_TEX_UNITS]{};
	// uniforms
	uint8 uniformMode{0}; // LATTE_DECOMPILER_UNIFORM_MODE_*
	uint64 uniformDataHash64[2]{0}; // last uploaded data, to skip redundant updates
	std::vector<LatteDecompilerRemappedUniformEntry_t> list_remappedUniformEntries;
	// textures
	std::bitset<LATTE_NUM_MAX_TEX_UNITS> textureUnitMask2;
	uint16 textureUnitSamplerAssignment[LATTE_NUM_MAX_TEX_UNITS]{ 0 }; // LATTE_DECOMPILER_SAMPLER_NONE if unassigned
	bool textureUsesDepthCompare[LATTE_NUM_MAX_TEX_UNITS]{};
	uint8 textureRenderTargetIndex[LATTE_NUM_MAX_TEX_UNITS];

	// pixel shaders: one bit per color attachment that is written
	uint32 pixelColorOutputMask{ 0 };
	// pixel shaders: depth is exported
	bool depthMask{ false };
	// ring buffer parameters (vertex -> geometry stage)
	uint32 ringParameterCount{ 0 };
	uint32 ringParameterCountFromPrevStage{ 0 }; // geometry shaders: count written by the vertex shader
	// transform feedback
	std::bitset<LATTE_NUM_STREAMOUT_BUFFER> streamoutBufferWriteMask;
	bool hasStreamoutBufferWrite{ false };
	// generated source
	class StringBuf* strBuf_shaderSource{ nullptr };
	// compiled host shader
	RendererShader* shader{ nullptr };
	bool isCustomShader{ false };

	uint32 outputParameterMask{ 0 };
	// bindings of the active backend
	LatteDecompilerShaderResourceMapping resourceMapping{};
	// uniform locations / cached values (filled by the renderer)
	struct
	{
		sint32 loc_remapped;
		sint32 loc_uniformRegister;
		sint32 count_uniformRegister;
		sint32 loc_windowSpaceToClipSpaceTransform;
		sint32 loc_alphaTestRef;
		sint32 loc_pointSize;
		sint32 loc_fragCoordScale;
		sint32 loc_framebufferFetchSize[LATTE_NUM_MAX_TEX_UNITS];
		std::vector<LatteUniformTextureScaleEntry_t> list_ufTexRescale;
		float ufCurrentValueAlphaTestRef;
		float ufCurrentValueFragCoordScale[2];
		sint32 loc_verticesPerInstance;
		sint32 loc_streamoutBufferBase[LATTE_NUM_STREAMOUT_BUFFER];
		uint32 uniformRangeSize; // size of the whole uniform variable block
	}uniform{ 0 };
	// remapped constants grouped for fast uploads
	struct _RemappedUniformBufferGroup
	{
		_RemappedUniformBufferGroup(uint16 bufferId, uint16 _kcacheBankIdOffset) : bufferId(bufferId), kcacheBankIdOffset(_kcacheBankIdOffset) {};
		uint16 bufferId;
		uint16 kcacheBankIdOffset;
		std::vector<LatteFastAccessRemappedUniformEntry_buffer_t> entries;
	};
	std::vector<LatteFastAccessRemappedUniformEntry_register_t>	list_remappedUniformEntries_register;
	std::vector<_RemappedUniformBufferGroup> list_remappedUniformEntries_bufferGroups;
	// shader state cache keys referencing this shader
	std::vector<uint64> m_shaderStateCacheKeys;
};

// byte offsets of the helper uniforms inside the uniform variable block (-1 = absent)
struct LatteDecompilerOutputUniformOffsets
{
	sint32 offset_remapped;
	sint32 offset_uniformRegister;
	sint32 count_uniformRegister; // vec4 entries
	sint32 offset_alphaTestRef;
	sint32 offset_pointSize;
	sint32 offset_fragCoordScale;
	sint32 offset_framebufferFetchSize[LATTE_NUM_MAX_TEX_UNITS];
	sint32 offset_windowSpaceToClipSpaceTransform;
	sint32 offset_texScale[LATTE_NUM_MAX_TEX_UNITS];
	sint32 offset_verticesPerInstance{-1};
	sint32 offset_streamoutBufferBase[LATTE_NUM_STREAMOUT_BUFFER]{ -1, -1, -1, -1 };
	sint32 offset_endOfBlock; // total size of the block

	LatteDecompilerOutputUniformOffsets()
	{
		// -1 marks a uniform the shader does not use
		for (sint32* unused : {&offset_remapped, &offset_uniformRegister, &offset_alphaTestRef, &offset_pointSize,
							   &offset_fragCoordScale, &offset_windowSpaceToClipSpaceTransform})
			*unused = -1;
		std::fill(std::begin(offset_framebufferFetchSize), std::end(offset_framebufferFetchSize), -1);
		std::fill(std::begin(offset_texScale), std::end(offset_texScale), -1);
		count_uniformRegister = 0;
		offset_endOfBlock = 0;
	}
};

struct LatteDecompilerOptions
{
	bool usesGeometryShader{ false };
	// emulate the 0 * x = 0 rule of the non-IEEE multiply
	bool strictMul{};
	// Vulkan: transform feedback via storage buffer
	bool useTFViaSSBO{ false };
	struct
	{
		bool hasRoundingModeRTEFloat32{ false };
	}spirvInstrinsics;
};

struct LatteDecompilerOutput_t
{
	LatteDecompilerShader* shader;
	LatteConst::ShaderType shaderType;

	// texture units sampled by the program
	std::bitset<LATTE_NUM_MAX_TEX_UNITS> textureUnitMask;

	// transform feedback buffers written and their strides in bytes
	std::bitset<LATTE_NUM_STREAMOUT_BUFFER> streamoutBufferWriteMask;
	uint32 streamoutBufferStride[LATTE_NUM_STREAMOUT_BUFFER]{};

	// helper uniform layout per API
	LatteDecompilerOutputUniformOffsets uniformOffsetsGL;
	LatteDecompilerOutputUniformOffsets uniformOffsetsVK;
	// binding layout per API
	LatteDecompilerShaderResourceMapping resourceMappingGL;
	LatteDecompilerShaderResourceMapping resourceMappingVK;
};

struct LatteDecompilerSubroutineInfo;

void LatteDecompiler_DecompileVertexShader(uint64 shaderBaseHash, uint32* contextRegisters, uint8* programData, uint32 programSize, struct LatteFetchShader* fetchShader, LatteDecompilerOptions& options, LatteDecompilerOutput_t* output);
void LatteDecompiler_DecompileGeometryShader(uint64 shaderBaseHash, uint32* contextRegisters, uint8* programData, uint32 programSize, uint8* gsCopyProgramData, uint32 gsCopyProgramSize, uint32 vsRingParameterCount, LatteDecompilerOptions& options, LatteDecompilerOutput_t* output);
void LatteDecompiler_DecompilePixelShader(uint64 shaderBaseHash, uint32* contextRegisters, uint8* programData, uint32 programSize, LatteDecompilerOptions& options, LatteDecompilerOutput_t* output);

// geometry shader copy program (implemented in Core/LatteGSCopyShaderParser.cpp)

#define GPU7_COPY_SHADER_MAX_PARAMS	(32)

struct LatteGSCopyShaderStreamWrite_t
{
	uint8 bufferIndex;
	uint16 offset; // ring buffer offset of the source value
	uint32 exportArrayBase;
	uint32 memWriteArraySize;
	uint32 memWriteCompMask;
};

struct LatteParsedGSCopyShader
{
	struct
	{
		uint16 offset;
		uint16 gprIndex;
		uint8  exportType;
		uint8  exportParam;
	}paramMapping[GPU7_COPY_SHADER_MAX_PARAMS];
	sint32 numParam;
	std::vector<LatteGSCopyShaderStreamWrite_t> list_streamWrites;
};

LatteParsedGSCopyShader* LatteGSCopyShaderParser_parse(uint8* programData, uint32 programSize);
bool LatteGSCopyShaderParser_getExportTypeByOffset(LatteParsedGSCopyShader* shaderContext, uint32 offset, uint32* exportType, uint32* exportParam);
