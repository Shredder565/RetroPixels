// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "GX2.h"
#include "GX2_Shader.h"
#include "GX2_Misc.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/LatteInstructions.h"

namespace GX2
{
	using namespace Latte;
	using ENDIAN = LatteConst::VertexFetchEndianMode;

	/* fetch shader generation */

	// GX2 attribute formats (low 6 bits of the format value) and their GPU fetch format / default byte swap
	struct AttribFormatInfo
	{
		uint8 fetchFormat;
		ENDIAN defaultSwap;
	};

	static constexpr AttribFormatInfo kAttribFormats[] =
	{
		/* 0x00 */ { 0x01, ENDIAN::SWAP_NONE }, /* 0x01 */ { 0x02, ENDIAN::SWAP_NONE },
		/* 0x02 */ { 0x05, ENDIAN::SWAP_U16 }, /* 0x03 */ { 0x06, ENDIAN::SWAP_U16 },
		/* 0x04 */ { 0x07, ENDIAN::SWAP_NONE }, /* 0x05 */ { 0x0D, ENDIAN::SWAP_U32 },
		/* 0x06 */ { 0x0E, ENDIAN::SWAP_U32 }, /* 0x07 */ { 0x0F, ENDIAN::SWAP_U16 },
		/* 0x08 */ { 0x10, ENDIAN::SWAP_U16 }, /* 0x09 */ { 0x16, ENDIAN::SWAP_U32 },
		/* 0x0A */ { 0x1A, ENDIAN::SWAP_NONE }, /* 0x0B */ { 0x19, ENDIAN::SWAP_U32 },
		/* 0x0C */ { 0x1D, ENDIAN::SWAP_U32 }, /* 0x0D */ { 0x1E, ENDIAN::SWAP_U32 },
		/* 0x0E */ { 0x1F, ENDIAN::SWAP_U16 }, /* 0x0F */ { 0x20, ENDIAN::SWAP_U16 },
		/* 0x10 */ { 0x2F, ENDIAN::SWAP_U32 }, /* 0x11 */ { 0x30, ENDIAN::SWAP_U32 },
		/* 0x12 */ { 0x22, ENDIAN::SWAP_U32 }, /* 0x13 */ { 0x23, ENDIAN::SWAP_U32 },
	};

	static const AttribFormatInfo* _LookupAttribFormat(uint32 attribFormat)
	{
		const uint32 idx = attribFormat & 0x3F;
		if (idx >= std::size(kAttribFormats))
			return nullptr;
		return kAttribFormats + idx;
	}

	// guest layout of GX2AttribStream; plain uint32 fields hold raw big endian values
	struct GX2AttribDescription
	{
		/* +0x00 */ uint32 location; // semantic id
		/* +0x04 */ uint32 buffer;
		/* +0x08 */ uint32be offset;
		/* +0x0C */ uint32 format; // bits 0-5: format, 0x100: integer, 0x200: signed, 0x800: scaled
		/* +0x10 */ uint32 indexType; // 0: per vertex, 1: per instance
		/* +0x14 */ uint32 aluDivisor;
		/* +0x18 */ uint32 destSel;
		/* +0x1C */ betype<ENDIAN> endianSwap;
	};

	static_assert(sizeof(betype<ENDIAN>) == 4);
	static_assert(sizeof(GX2AttribDescription) == 0x20);

	// Program layout: CF instructions (one VTX_TC per 16 attributes, then RETURN), padded to 16 bytes,
	// followed by one 16 byte fetch clause instruction per attribute.
	static constexpr uint32 kAttribsPerClause = 16;

	static uint32 _FetchShaderCFBytes(uint32 attributeCount)
	{
		const uint32 cfInstructions = (attributeCount + kAttribsPerClause - 1) / kAttribsPerClause + 1;
		return (cfInstructions * 8 + 15) & ~15u;
	}

	static uint32 _FetchShaderClauseBytes(uint32 attributeCount)
	{
		return attributeCount * 16;
	}

	static void _EmitFetchShaderCF(uint8* out, uint32 attributeCount)
	{
		const uint32 clauseStart = _FetchShaderCFBytes(attributeCount);
		for (uint32 first = 0; first < attributeCount; first += kAttribsPerClause)
		{
			LatteCFInstruction_DEFAULT vtx;
			vtx.setField_Opcode(LatteCFInstruction::INST_VTX_TC);
			vtx.setField_COUNT(std::min(attributeCount - first, kAttribsPerClause));
			vtx.setField_ADDR(clauseStart + first * 16);
			memcpy(out, &vtx, sizeof(LatteCFInstruction));
			out += sizeof(LatteCFInstruction);
		}
		LatteCFInstruction_DEFAULT ret;
		ret.setField_Opcode(LatteCFInstruction::INST_RETURN);
		ret.setField_BARRIER(true);
		memcpy(out, &ret, sizeof(LatteCFInstruction));
		// the alignment padding after the last CF instruction is left untouched
	}

	// picks the SRC_SEL_X encoding for an instanced attribute. 1 and 2 select one of the two step
	// rate (divisor) registers, 3 means "advance every instance". New divisors are added to the table.
	static uint32 _InstanceStepSelect(GX2FetchShader* fs, uint32 divisor)
	{
		if (divisor == 1)
			return 3;
		cemu_assert_debug(divisor != 0);
		uint32 used = _swapEndianU32(fs->divisorCount);
		for (uint32 slot = 0; slot < used; slot++)
		{
			if (fs->divisors[slot] == divisor)
				return slot == 0 ? 1 : 2;
		}
		if (used >= 2)
		{
			cemu_assert_debug(false); // both step rate registers are taken
			return 0;
		}
		fs->divisors[used] = divisor;
		fs->divisorCount = _swapEndianU32(used + 1);
		return used == 0 ? 1 : 2;
	}

	static void _EmitFetchClauses(GX2FetchShader* fs, uint8* out, uint32 attributeCount, GX2AttribDescription* attribs)
	{
		using VTX = LatteClauseInstruction_VTX;
		for (uint32 i = 0; i < attributeCount; i++)
		{
			const GX2AttribDescription& attr = attribs[i];
			const uint32 format = _swapEndianU32(attr.format);
			const uint32 destSel = _swapEndianU32(attr.destSel);
			const uint32 location = _swapEndianU32(attr.location);
			const uint32 bufferIndex = _swapEndianU32(attr.buffer);
			const uint32 indexType = _swapEndianU32(attr.indexType);
			const uint32 divisor = _swapEndianU32(attr.aluDivisor);
			const AttribFormatInfo* fmtInfo = _LookupAttribFormat(format);

			ENDIAN swap = attr.endianSwap;
			if (swap == ENDIAN::SWAP_DEFAULT)
			{
				if (fmtInfo)
					swap = fmtInfo->defaultSwap;
				else
				{
					cemu_assert_suspicious();
					swap = ENDIAN::SWAP_NONE;
				}
			}

			cemu_assert_debug(indexType <= 1);
			uint32 srcSel = 0; // per vertex index
			if (indexType == 1)
				srcSel = _InstanceStepSelect(fs, divisor);

			// unknown formats map to fetch format 0 (invalid). The table lookup used to read past its end here.
			const uint32 fetchFormat = fmtInfo ? fmtInfo->fetchFormat : 0;
			uint32 numFormat = 0; // normalized
			if (format & 0x800)
				numFormat = 2; // scaled
			else if (format & 0x100)
				numFormat = 1; // integer

			VTX instr;
			instr.setField_VTX_INST(VTX::VTX_INST::_VTX_INST_SEMANTIC);
			instr.setFieldSEM_SEMANTIC_ID(location & 0xFF);
			instr.setField_BUFFER_ID(bufferIndex + 0xA0); // vertex buffers start at fetch resource 0xA0
			instr.setField_FETCH_TYPE((LatteConst::VertexFetchType2)indexType);
			instr.setField_SRC_SEL_X((VTX::SRC_SEL)srcSel);
			instr.setField_DATA_FORMAT((E_HWFMT)fetchFormat);
			instr.setField_NUM_FORMAT_ALL((VTX::NUM_FORMAT_ALL)numFormat);
			instr.setField_OFFSET(attr.offset);
			if (format & 0x200)
				instr.setField_FORMAT_COMP_ALL(VTX::FORMAT_COMP::COMP_SIGNED);
			instr.setField_ENDIAN_SWAP(swap);
			for (uint32 c = 0; c < 4; c++)
				instr.setField_DST_SEL(c, (VTX::DST_SEL)((destSel >> (24 - c * 8)) & 7));
			memcpy(out, &instr, 16);
			out += 16;
		}
	}

	uint32 GX2CalcFetchShaderSizeEx(uint32 attributeCount, GX2FetchShader::FetchShaderType fetchShaderType, uint32 tessellationMode)
	{
		// only the non-tessellated variant is implemented
		cemu_assert_debug(fetchShaderType == GX2FetchShader::FetchShaderType::NO_TESSELATION);
		cemu_assert_debug(tessellationMode == 0);
		return _FetchShaderCFBytes(attributeCount) + _FetchShaderClauseBytes(attributeCount);
	}

	void GX2InitFetchShaderEx(GX2FetchShader* fetchShader, void* programBufferOut, uint32 attributeCount, GX2AttribDescription* attributeDescription, GX2FetchShader::FetchShaderType fetchShaderType, uint32 tessellationMode)
	{
		cemu_assert_debug(fetchShaderType == GX2FetchShader::FetchShaderType::NO_TESSELATION);
		cemu_assert_debug(tessellationMode == 0);
		memset(fetchShader, 0, sizeof(GX2FetchShader));
		fetchShader->attribCount = _swapEndianU32(attributeCount);
		fetchShader->shaderPtr = _swapEndianU32(memory_getVirtualOffsetFromPointer(programBufferOut));

		uint8* program = (uint8*)programBufferOut;
		const uint32 cfBytes = _FetchShaderCFBytes(attributeCount);
		_EmitFetchShaderCF(program, attributeCount);
		_EmitFetchClauses(fetchShader, program + cfBytes, attributeCount, attributeDescription);

		fetchShader->shaderSize = _swapEndianU32(cfBytes + _FetchShaderClauseBytes(attributeCount));
		fetchShader->reg_SQ_PGM_RESOURCES_FS = LATTE_SQ_PGM_RESOURCES_FS().set_NUM_GPRS(2);
	}

	/* shader queries */

	uint32 GX2GetVertexShaderGPRs(GX2VertexShader* vertexShader)
	{
		return vertexShader->regs.SQ_PGM_RESOURCES_VS.value().get_NUM_GPRS();
	}

	uint32 GX2GetVertexShaderStackEntries(GX2VertexShader* vertexShader)
	{
		return vertexShader->regs.SQ_PGM_RESOURCES_VS.value().get_NUM_STACK_ENTRIES();
	}

	uint32 GX2GetPixelShaderGPRs(GX2PixelShader_t* pixelShader)
	{
		return _swapEndianU32(pixelShader->regs[0]) & 0xFF;
	}

	uint32 GX2GetPixelShaderStackEntries(GX2PixelShader_t* pixelShader)
	{
		// The value would be bits 8-15 of regs[0]. PixelU has always reported 0 here and
		// games feed the result straight back into GX2SetShaderModeEx, so 0 is kept to
		// leave the emitted command stream unchanged.
		return 0;
	}

	/* binding shaders */

	void GX2SetFetchShader(GX2FetchShader* fetchShaderPtr)
	{
		const MPTR program = _swapEndianU32(fetchShaderPtr->shaderPtr);
		const uint32 size = _swapEndianU32(fetchShaderPtr->shaderSize);
		cemu_assert_debug((program & 0xFF) == 0);
		GX2ReserveCmdSpace(11);
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 6), REGADDR::SQ_PGM_START_FS - 0xA000,
			program >> 8, size >> 3, 0x10000, 0x10000, fetchShaderPtr->reg_SQ_PGM_RESOURCES_FS,
			pm4HeaderType3(IT_SET_CONTEXT_REG, 3), REGADDR::VGT_INSTANCE_STEP_RATE_0 - 0xA000,
			fetchShaderPtr->divisors[0], fetchShaderPtr->divisors[1]);
	}


	void GX2SetVertexShader(GX2VertexShader* vertexShader)
	{
		const bool gsMode = vertexShader->shaderMode == GX2_SHADER_MODE::GEOMETRY_SHADER;
		const bool streamOut = vertexShader->usesStreamOut != 0;
		const uint32 outIdCount = std::min<uint32>(vertexShader->regs.vsOutIdTableSize, 10);
		const uint32 semanticCount = vertexShader->regs.semanticTableSize;

		// space estimate; a bit larger than what is actually written
		uint32 reserve = 31 + (gsMode ? 7 : 18 + outIdCount + (streamOut ? 14 : 0));
		if (semanticCount > 0)
			reserve += 5 + semanticCount;
		GX2ReserveCmdSpace(reserve);

		const bool hasDirectProgram = (bool)vertexShader->shaderPtr;
		const MPTR program = hasDirectProgram ? vertexShader->shaderPtr.GetMPTR() : vertexShader->rBuffer.GetVirtualAddr();
		const uint32 programSize = hasDirectProgram ? (uint32)vertexShader->shaderSize : vertexShader->rBuffer.GetSize();
		cemu_assert_debug(program != 0);
		cemu_assert_debug(programSize != 0);

		// with a geometry shader active the vertex shader runs as export shader (ES) and the
		// VS output registers are set up by GX2SetGeometryShader instead
		const uint32 startReg = gsMode ? REGADDR::SQ_PGM_START_ES : REGADDR::SQ_PGM_START_VS;
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 6), startReg - 0xA000,
			memory_virtualToPhysical(program) >> 8, programSize >> 3, 0x100000, 0x100000,
			vertexShader->regs.SQ_PGM_RESOURCES_VS);

		if (!gsMode)
		{
			const auto& r = vertexShader->regs;
			gx2WriteGather_submit(
				pm4HeaderType3(IT_SET_CONTEXT_REG, 2), REGADDR::VGT_PRIMITIVEID_EN - 0xA000, r.VGT_PRIMITIVEID_EN,
				pm4HeaderType3(IT_SET_CONTEXT_REG, 2), REGADDR::SPI_VS_OUT_CONFIG - 0xA000, r.SPI_VS_OUT_CONFIG,
				pm4HeaderType3(IT_SET_CONTEXT_REG, 2), REGADDR::PA_CL_VS_OUT_CNTL - 0xA000, r.PA_CL_VS_OUT_CNTL);
			cemu_assert_debug(!r.SPI_VS_OUT_CONFIG.value().get_VS_PER_COMPONENT()); // the GPU side doesn't support it

			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + outIdCount), REGADDR::SPI_VS_OUT_ID_0 - 0xA000);
			for (uint32 i = 0; i < outIdCount; i++)
				gx2WriteGather_submit(r.LATTE_SPI_VS_OUT_ID_N[i]);

			if (streamOut)
			{
				// strides are given in bytes, the registers take dwords
				const uint32 strideRegs[GX2_MAX_STREAMOUT_BUFFERS] = { REGADDR::VGT_STRMOUT_VTX_STRIDE_0, REGADDR::VGT_STRMOUT_VTX_STRIDE_1, REGADDR::VGT_STRMOUT_VTX_STRIDE_2, REGADDR::VGT_STRMOUT_VTX_STRIDE_3 };
				for (uint32 i = 0; i < GX2_MAX_STREAMOUT_BUFFERS; i++)
					gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), strideRegs[i] - 0xA000, (uint32)vertexShader->streamOutVertexStride[i] >> 2);
			}
		}

		if (semanticCount > 0)
		{
			// reset all semantic mappings, then upload the shader's table (at most 32 entries)
			const uint32 count = std::min<uint32>(semanticCount, 32);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), REGADDR::SQ_VTX_SEMANTIC_CLEAR - 0xA000, 0xFFFFFFFFu,
				pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + count), REGADDR::SQ_VTX_SEMANTIC_0 - 0xA000);
			gx2WriteGather_submitU32AsLEArray((uint32*)vertexShader->regs.SQ_VTX_SEMANTIC_N, count);
		}
	}

	/* uniform registers (ALU constants) */

	// constants are vec4s: the offset and size are in dwords, PS constants start at 0, VS ones at 0x400
	static void _UploadAluConstants(uint32 stageBase, uint32 offset, uint32be* values, uint32 sizeInU32s)
	{
		if (offset & 0x8000)
		{
			// loop constants are addressed this way; not supported
			cemuLog_logDebugOnce(LogType::Force, "GX2Set*UniformReg(): loop constant or invalid offset 0x{:x} ignored", offset);
			return;
		}
		if (offset + sizeInU32s > 0x400)
			cemuLog_logOnce(LogType::APIErrors, "GX2Set*UniformReg(): range exceeds the 1024 constant registers (offset {} size {})", offset, sizeInU32s);
		if (sizeInU32s % 4 != 0)
		{
			cemuLog_logOnce(LogType::APIErrors, "GX2Set*UniformReg(): size {} is not a multiple of 4", sizeInU32s);
			sizeInU32s -= sizeInU32s % 4;
		}
		GX2ReserveCmdSpace(2 + sizeInU32s);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_ALU_CONST, 1 + sizeInU32s), stageBase + offset);
		gx2WriteGather_submitU32AsLEArray((uint32*)values, sizeInU32s); // already big endian
	}

	void GX2SetVertexUniformReg(uint32 offset, uint32 sizeInU32s, uint32be* values)
	{
		_UploadAluConstants(0x400, offset, values, sizeInU32s);
	}

	void GX2SetPixelUniformReg(uint32 offset, uint32 sizeInU32s, uint32be* values)
	{
		_UploadAluConstants(0, offset, values, sizeInU32s);
	}

	/* shader mode */

	void GX2SetShaderModeEx(GX2_SHADER_MODE mode, uint32 shaderGprsVS, uint32 shaderStackVS, uint32 shaderGprsGS, uint32 shaderStackGS, uint32 shaderGprsPS, uint32 shaderStackPS)
	{
		if ((uint32)mode > (uint32)GX2_SHADER_MODE::COMPUTE_SHADER)
		{
			cemu_assert_suspicious();
			return;
		}
		const bool compute = mode == GX2_SHADER_MODE::COMPUTE_SHADER;
		const bool geometry = mode == GX2_SHADER_MODE::GEOMETRY_SHADER;
		// newer SDKs also program SPI_THREAD_GROUPING
		const bool threadGrouping = !compute && coreinit::__OSGetProcessSDKVersion() >= 21104;
		const uint32 baseWords = compute ? 26 : (geometry ? 8 : 11);
		GX2ReserveCmdSpace(baseWords + (threadGrouping ? 3 : 0));

		if (!geometry) // in geometry mode VGT_GS_MODE comes with the geometry shader
		{
			LATTE_VGT_GS_MODE gsModeReg;
			if (compute)
			{
				gsModeReg.set_MODE(LATTE_VGT_GS_MODE::E_MODE::SCENARIO_G);
				gsModeReg.set_COMPUTE_MODE(LATTE_VGT_GS_MODE::E_COMPUTE_MODE::ON);
				gsModeReg.set_PARTIAL_THD_AT_EOI(true);
			}
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), REGADDR::VGT_GS_MODE - LATTE_REG_BASE_CONTEXT, gsModeReg);
		}

		// thread priorities: compute favours ES (where compute runs), graphics favours PS
		LATTE_SQ_CONFIG sqConfig;
		sqConfig.set_DX9_CONSTS(mode == GX2_SHADER_MODE::UNIFORM_REGISTER);
		sqConfig.set_ALU_INST_PREFER_VECTOR(true);
		sqConfig.set_PS_PRIO(compute ? 0 : 3);
		sqConfig.set_VS_PRIO(compute ? 1 : 2);
		sqConfig.set_GS_PRIO(compute ? 2 : 1);
		sqConfig.set_ES_PRIO(compute ? 3 : 0);

		LATTE_SQ_GPR_RESOURCE_MGMT_1 gpr1;
		LATTE_SQ_GPR_RESOURCE_MGMT_2 gpr2;
		LATTE_SQ_STACK_RESOURCE_MGMT_1 stack1;
		LATTE_SQ_STACK_RESOURCE_MGMT_2 stack2;
		uint32 threadMgmt;
		if (compute)
		{
			gpr2.set_NUM_ES_GPRS(0xF8);
			stack2.set_NUM_ES_STACK_ENTRIES(0x100);
			threadMgmt = 0xBD010101;
		}
		else if (geometry)
		{
			// the VS runs as ES, the VS slot runs the GS copy shader
			gpr1.set_NUM_PS_GPRS(shaderGprsPS).set_NUM_VS_GPRS(0x40);
			gpr2.set_NUM_GS_GPRS(shaderGprsGS).set_NUM_ES_GPRS(shaderGprsVS);
			stack1.set_NUM_PS_STACK_ENTRIES(shaderStackPS);
			stack2.set_NUM_GS_STACK_ENTRIES(shaderStackGS).set_NUM_ES_STACK_ENTRIES(shaderStackVS);
			threadMgmt = 0x1C08207C;
		}
		else
		{
			gpr1.set_NUM_PS_GPRS(shaderGprsPS).set_NUM_VS_GPRS(shaderGprsVS);
			stack1.set_NUM_PS_STACK_ENTRIES(shaderStackPS).set_NUM_VS_STACK_ENTRIES(shaderStackVS);
			threadMgmt = 0x04043088;
		}
		// SQ_CONFIG, GPR_RESOURCE_MGMT_1/2, THREAD_RESOURCE_MGMT, STACK_RESOURCE_MGMT_1/2 are consecutive
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONFIG_REG, 7), REGADDR::SQ_CONFIG - LATTE_REG_BASE_CONFIG,
			sqConfig, gpr1.getRawValue() | 0x40000000, gpr2, threadMgmt, stack1, stack2);

		if (compute)
		{
			// no ES->GS ring, GS->VS ring item size 1, stream out off
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONFIG_REG, 5), REGADDR::SQ_ESGS_RING_BASE - LATTE_REG_BASE_CONFIG, 0, 0xFFFFFF, 0, 0xFFFFFF);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), REGADDR::SQ_ESGS_RING_ITEMSIZE - LATTE_REG_BASE_CONTEXT, 0);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), REGADDR::SQ_GSVS_RING_ITEMSIZE - LATTE_REG_BASE_CONTEXT, 1);
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), REGADDR::VGT_STRMOUT_EN - LATTE_REG_BASE_CONTEXT, 0);
		}
		else if (threadGrouping)
		{
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_ALL_CONTEXTS, 2), REGADDR::SPI_THREAD_GROUPING - LATTE_REG_BASE_CONTEXT, 1);
		}

		if (mode != GX2_SHADER_MODE::UNIFORM_REGISTER)
			GX2Invalidate(GX2InvalidationFlag::GPU_SHADER, MPTR_NULL, 0xFFFFFFFF);
	}

	void GX2ShaderInit()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("gx2", fn, LogType::GX2)
		EXPORT_FN(GX2CalcFetchShaderSizeEx); EXPORT_FN(GX2InitFetchShaderEx);

		EXPORT_FN(GX2GetVertexShaderGPRs); EXPORT_FN(GX2GetVertexShaderStackEntries); EXPORT_FN(GX2GetPixelShaderGPRs);
		EXPORT_FN(GX2GetPixelShaderStackEntries); EXPORT_FN(GX2SetFetchShader); EXPORT_FN(GX2SetVertexShader);

		EXPORT_FN(GX2SetVertexUniformReg); EXPORT_FN(GX2SetPixelUniformReg);

		EXPORT_FN(GX2SetShaderModeEx);
#undef EXPORT_FN
	}
}
