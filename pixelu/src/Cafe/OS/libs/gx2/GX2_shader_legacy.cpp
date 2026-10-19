// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"

#include "GX2.h"
#include "GX2_Shader.h"

// Pixel/geometry/compute shader binding and uniform blocks. These exports use the register
// level (hCPU) calling convention and return 0 in r3.

namespace
{
	// guest register arrays hold big endian values
	inline uint32 GuestReg(uint32 rawBE)
	{
		return _swapEndianU32(rawBE);
	}

	// one SET_CONTEXT_REG packet for a single register (3 words)
	inline void EmitContextReg(uint32 reg, uint32 value)
	{
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), reg - 0xA000, value);
	}

	// SQ_PGM_START_xx, SQ_PGM_SIZE_xx, two ring sizes and SQ_PGM_RESOURCES_xx (7 words)
	inline void EmitProgramStart(uint32 startReg, MPTR program, uint32 programSize, uint32 resources)
	{
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 6), startReg - 0xA000,
			memory_virtualToPhysical(program) >> 8, programSize >> 3, 0x100000, 0x100000, resources);
	}

	// regs[] layout of GX2PixelShader_t
	enum PSReg : uint32
	{
		PS_PGM_RESOURCES = 0,
		PS_IN_CONTROL_0 = 2,
		PS_IN_CONTROL_1 = 3,
		PS_NUM_INPUTS = 4,
		PS_INPUT_CNTL_FIRST = 5, // 32 entries
		PS_CB_SHADER_MASK = 37,
		PS_CB_SHADER_CONTROL = 38,
		PS_DB_SHADER_CONTROL = 39,
		PS_SPI_INPUT_Z = 40,
	};

	// regs[] layout of GX2GeometryShader_t
	enum GSReg : uint32
	{
		GS_PGM_RESOURCES_GS = 0,
		GS_OUT_PRIM_TYPE = 1,
		GS_MODE = 2,
		GS_PA_CL_VS_OUT_CNTL = 3,
		GS_PGM_RESOURCES_VS = 4, // copy shader
		GS_VERT_ITEMSIZE = 5,
		GS_SPI_VS_OUT_CONFIG = 6,
		GS_NUM_OUT_IDS = 7,
		GS_OUT_ID_FIRST = 8, // 10 entries
		GS_STRMOUT_BUFFER_EN = 18,
	};

	void SetPixelShader(GX2PixelShader_t* ps)
	{
		const uint32 inputCount = std::min<uint32>(GuestReg(ps->regs[PS_NUM_INPUTS]), 32);
		GX2::GX2ReserveCmdSpace(26 + inputCount);

		const MPTR directProgram = _swapEndianU32(ps->shaderPtr);
		const MPTR program = directProgram != MPTR_NULL ? directProgram : ps->rBuffer.GetVirtualAddr();
		const uint32 programSize = directProgram != MPTR_NULL ? _swapEndianU32(ps->shaderSize) : ps->rBuffer.GetSize();

		EmitProgramStart(mmSQ_PGM_START_PS, program, programSize, GuestReg(ps->regs[PS_PGM_RESOURCES]));
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 3), mmSPI_PS_IN_CONTROL_0 - 0xA000,
			GuestReg(ps->regs[PS_IN_CONTROL_0]), GuestReg(ps->regs[PS_IN_CONTROL_1]));

		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + inputCount), mmSPI_PS_INPUT_CNTL_0 - 0xA000);
		for (uint32 i = 0; i < inputCount; i++)
			gx2WriteGather_submit(GuestReg(ps->regs[PS_INPUT_CNTL_FIRST + i]));

		EmitContextReg(mmCB_SHADER_MASK, GuestReg(ps->regs[PS_CB_SHADER_MASK]));
		EmitContextReg(mmCB_SHADER_CONTROL, GuestReg(ps->regs[PS_CB_SHADER_CONTROL]));
		EmitContextReg(mmDB_SHADER_CONTROL, GuestReg(ps->regs[PS_DB_SHADER_CONTROL]));
		EmitContextReg(mmSPI_INPUT_Z, GuestReg(ps->regs[PS_SPI_INPUT_Z]));
	}

	void SetGeometryShader(GX2GeometryShader_t* gs)
	{
		const uint32 outIdCount = std::min<uint32>(GuestReg(gs->regs[GS_NUM_OUT_IDS]), 10);
		const bool streamOut = _swapEndianU32(gs->useStreamout) != 0;
		uint32 reserve = 38;
		if (outIdCount != 0)
			reserve += 2 + outIdCount;
		if (streamOut)
			reserve += 14;
		GX2::GX2ReserveCmdSpace(reserve);

		// geometry program (GS stage)
		EmitProgramStart(mmSQ_PGM_START_GS, gs->GetGeometryProgramAddr(),
			_swapEndianU32(gs->shaderPtr) != MPTR_NULL ? _swapEndianU32(gs->shaderSize) : gs->rBuffer.GetSize(),
			GuestReg(gs->regs[GS_PGM_RESOURCES_GS]));
		EmitContextReg(mmVGT_GS_OUT_PRIM_TYPE, GuestReg(gs->regs[GS_OUT_PRIM_TYPE]));
		EmitContextReg(Latte::REGADDR::VGT_GS_MODE, GuestReg(gs->regs[GS_MODE]));
		EmitContextReg(mmSQ_PGM_RESOURCES_GS, GuestReg(gs->regs[GS_PGM_RESOURCES_GS]));
		EmitContextReg(mmSQ_GS_VERT_ITEMSIZE, GuestReg(gs->regs[GS_VERT_ITEMSIZE]));
		if (streamOut)
		{
			const uint32 strideRegs[4] = { mmVGT_STRMOUT_VTX_STRIDE_0, mmVGT_STRMOUT_VTX_STRIDE_1, mmVGT_STRMOUT_VTX_STRIDE_2, mmVGT_STRMOUT_VTX_STRIDE_3 };
			for (uint32 i = 0; i < 4; i++)
				EmitContextReg(strideRegs[i], _swapEndianU32(gs->streamoutStride[i]) >> 2); // bytes -> dwords
		}
		EmitContextReg(mmVGT_STRMOUT_BUFFER_EN, GuestReg(gs->regs[GS_STRMOUT_BUFFER_EN]));

		// the copy shader runs in the VS stage and moves GS output from the ring to the rasterizer
		const MPTR copyProgram = gs->GetCopyProgramAddr();
		const uint32 copyProgramSize = _swapEndianU32(gs->copyShaderPtr) != MPTR_NULL ? _swapEndianU32(gs->copyShaderSize) : gs->rBufferCopyProgram.GetSize();
		cemu_assert_debug((copyProgram >> 8) != 0);
		cemu_assert_debug((copyProgramSize >> 3) != 0);
		EmitProgramStart(mmSQ_PGM_START_VS, copyProgram, copyProgramSize, GuestReg(gs->regs[GS_PGM_RESOURCES_VS]));
		EmitContextReg(mmPA_CL_VS_OUT_CNTL, GuestReg(gs->regs[GS_PA_CL_VS_OUT_CNTL]));
		if (outIdCount != 0)
		{
			gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 1 + outIdCount), mmSPI_VS_OUT_ID_0 - 0xA000);
			for (uint32 i = 0; i < outIdCount; i++)
				gx2WriteGather_submit(GuestReg(gs->regs[GS_OUT_ID_FIRST + i]));
		}
		EmitContextReg(mmSPI_VS_OUT_CONFIG, GuestReg(gs->regs[GS_SPI_VS_OUT_CONFIG]));
		EmitContextReg(mmSQ_GSVS_RING_ITEMSIZE, _swapEndianU32(gs->ringItemsize) & 0x7FFF);
	}
}

void gx2Export_GX2SetPixelShader(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetPixelShader(0x{:08x})", hCPU->gpr[3]);
	SetPixelShader((GX2PixelShader_t*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]));
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2SetGeometryShader(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetGeometryShader(0x{:08x})", hCPU->gpr[3]);
	SetGeometryShader((GX2GeometryShader_t*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]));
	osLib_returnFromFunction(hCPU, 0);
}

// guest layout of GX2ComputeShader
struct GX2ComputeShader
{
	/* +0x00 */ uint32be regs[12]; // [0]: SQ_PGM_RESOURCES_ES
	/* +0x30 */ uint32be programSize;
	/* +0x34 */ uint32be programPtr;
	/* +0x38 */ uint32be ukn38;
	/* +0x3C */ uint32be ukn3C;
	/* +0x40 */ uint32be ukn40[8];
	/* +0x60 */ uint32be workgroupSizeX;
	/* +0x64 */ uint32be workgroupSizeY;
	/* +0x68 */ uint32be workgroupSizeZ;
	/* +0x6C */ uint32be workgroupSizeSpecial;
	/* +0x70 */ uint32be ukn70;
	/* +0x74 */ GX2RBuffer rBuffer;
};

static_assert(offsetof(GX2ComputeShader, programSize) == 0x30);
static_assert(offsetof(GX2ComputeShader, workgroupSizeX) == 0x60);
static_assert(offsetof(GX2ComputeShader, rBuffer) == 0x74);

void gx2Export_GX2SetComputeShader(PPCInterpreter_t* hCPU)
{
	ppcDefineParamTypePtr(computeShader, GX2ComputeShader, 0);
	cemuLog_log(LogType::GX2, "GX2SetComputeShader(0x{:08x})", hCPU->gpr[3]);
	const bool direct = computeShader->programPtr != 0;
	const MPTR program = direct ? (MPTR)computeShader->programPtr : computeShader->rBuffer.GetVirtualAddr();
	const uint32 programSize = direct ? (uint32)computeShader->programSize : computeShader->rBuffer.GetSize();
	GX2::GX2ReserveCmdSpace(17);
	// compute shaders run in the ES stage; the remaining registers are not needed by our GPU emulation
	EmitProgramStart(mmSQ_PGM_START_ES, program, programSize, computeShader->regs[0]);
	osLib_returnFromFunction(hCPU, 0);
}

// uniform blocks are bound as buffer resources (7 words per slot)
static void _BindUniformBlock(uint32 firstSlotReg, uint32 index, MPTR addr, uint32 size)
{
	GX2::GX2ReserveCmdSpace(9);
	gx2WriteGather_submit(pm4HeaderType3(IT_SET_RESOURCE, 8),
		firstSlotReg - mmSQ_TEX_RESOURCE_WORD0 + index * 7,
		memory_virtualToPhysical(addr), size - 1, 0, 1, 0, 0, 0xC0000000);
}

// arguments: r3 = block index, r4 = size in bytes, r5 = address
void gx2Export_GX2SetVertexUniformBlock(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetVertexUniformBlock(0x{:08x},0x{:x},0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
	_BindUniformBlock(mmSQ_VTX_UNIFORM_BLOCK_START, hCPU->gpr[3], hCPU->gpr[5], hCPU->gpr[4]);
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2SetPixelUniformBlock(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetPixelUniformBlock(0x{:08x},0x{:x},0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
	_BindUniformBlock(mmSQ_PS_UNIFORM_BLOCK_START, hCPU->gpr[3], hCPU->gpr[5], hCPU->gpr[4]);
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2SetGeometryUniformBlock(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetGeometryUniformBlock(0x{:08x},0x{:x},0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
	_BindUniformBlock(mmSQ_GS_UNIFORM_BLOCK_START, hCPU->gpr[3], hCPU->gpr[5], hCPU->gpr[4]);
	osLib_returnFromFunction(hCPU, 0);
}

// arguments: r3 = GX2RBuffer*, r4 = block index, r5 = byte offset into the buffer
void gx2Export_GX2RSetVertexUniformBlock(PPCInterpreter_t* hCPU)
{
	GX2RBuffer* buffer = (GX2RBuffer*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
	const uint32 index = hCPU->gpr[4];
	const uint32 offset = hCPU->gpr[5];
	_BindUniformBlock(mmSQ_VTX_UNIFORM_BLOCK_START, index, buffer->GetVirtualAddr() + offset, buffer->GetSize() - offset);
	osLib_returnFromFunction(hCPU, 0);
}

// the GS rings need 16KiB per unit of item size
void gx2Export_GX2CalcGeometryShaderInputRingBufferSize(PPCInterpreter_t* hCPU)
{
	osLib_returnFromFunction(hCPU, hCPU->gpr[3] * 0x4000);
}

void gx2Export_GX2CalcGeometryShaderOutputRingBufferSize(PPCInterpreter_t* hCPU)
{
	osLib_returnFromFunction(hCPU, hCPU->gpr[3] * 0x4000);
}
