// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"

#include "Cafe/HW/Latte/Core/LattePM4.h"

#include "GX2_Shader.h"
#include "GX2_Command.h"
#include "GX2_State.h"
#include "Cafe/CafeSystem.h"

// Context states. A GX2ContextState is a block of guest memory that the GPU uses as shadow
// storage for its registers (state shadowing, enabled through CONTEXT_CONTROL). Switching to a
// context state means replaying LOAD_* packets that read the register ranges back from the
// shadow memory. Those LOAD packets are prebuilt into a display list inside the state.

namespace
{
	// one register range of a LOAD_* packet: offset from the register block base, count
	struct RegRange
	{
		uint32 offset;
		uint32 count;
	};

	// register ranges GX2 saves/restores per register block (the console driver's tables)
	constexpr RegRange kConfigRanges[] = // base 0x2000
	{
		{0x300, 0x6}, {0x900, 0x48}, {0x980, 0x48}, {0xA00, 0x48}, {0x310, 0xC},
		{0x542, 0x1}, {0x235, 0x1}, {0x232, 0x2}, {0x23A, 0x1}, {0x256, 0x1},
		{0x60C, 0x1}, {0x5C5, 0x1}, {0x2C8, 0x1}, {0x363, 0x1}, {0x404, 0x2},
	};

	constexpr RegRange kContextRanges[] = // base 0xA000
	{
		{0x0, 0x2}, {0x3, 0x3}, {0xA, 0x4}, {0x10, 0x38}, {0x50, 0x34},
		{0x8E, 0x4}, {0x94, 0x40}, {0x100, 0x9}, {0x10C, 0x3}, {0x10F, 0x60},
		{0x185, 0xA}, {0x191, 0x27}, {0x1E0, 0x9}, {0x200, 0x1}, {0x202, 0x7},
		{0xE0, 0x20}, {0x210, 0x29}, {0x250, 0x34}, {0x290, 0x1}, {0x292, 0x2},
		{0x2A1, 0x1}, {0x2A5, 0x1}, {0x2A8, 0x2}, {0x2AC, 0x3}, {0x2CA, 0x1},
		{0x2CC, 0x1}, {0x2CE, 0x1}, {0x300, 0x9}, {0x30C, 0x1}, {0x312, 0x1},
		{0x316, 0x2}, {0x343, 0x2}, {0x349, 0x3}, {0x34C, 0x2}, {0x351, 0x1},
		{0x37E, 0x6}, {0x2B4, 0x3}, {0x2B8, 0x3}, {0x2BC, 0x3}, {0x2C0, 0x3},
		{0x2C8, 0x1}, {0x29B, 0x1}, {0x8C, 0x1}, {0xD5, 0x1}, {0x284, 0xC},
	};

	constexpr RegRange kAluConstRanges[] = { {0, 0x800} }; // base 0xC000
	constexpr RegRange kLoopConstRanges[] = { {0, 0x60} }; // base 0xF880

	constexpr RegRange kResourceRanges[] = // base 0xE000
	{
		{0, 0x70}, {0x380, 0x70}, // pixel shader textures
		{0x460, 0x70}, {0x7E0, 0x70}, {0x8B9, 0x7}, {0x8C0, 0x70}, // vertex shader textures and buffers
		{0x930, 0x70}, {0xCB0, 0x70}, {0xD89, 0x7}, // geometry shader
	};

	constexpr RegRange kSamplerRanges[] = { {0, 0x36}, {0x36, 0x36}, {0x6C, 0x36} }; // base 0xF000

	// shadow memory of a register block: one dword per register, rounded up to 256 bytes
	constexpr uint32 ShadowWords(uint32 regCount)
	{
		return ((regCount * 4 + 0xFF) & ~0xFFu) / 4;
	}

	struct HwShadowState
	{
		uint32 config[ShadowWords(0xB00)];
		uint32 context[ShadowWords(0x400)];
		uint32 aluConst[ShadowWords(0x800)];
		uint32 loopConst[ShadowWords(0x60)];
		uint32 resource[ShadowWords(0xD9E)];
		uint32 sampler[ShadowWords(0xA2)];
	};
	static_assert(sizeof(HwShadowState) == 0x9800);

	// guest layout of GX2ContextState
	struct GX2ContextState
	{
		/* +0x0000 */ HwShadowState shadow;
		/* +0x9800 */ uint32 profilingEnabled; // raw BE
		/* +0x9804 */ uint32be restoreDLSize;
		/* +0x9808 */ uint8 ukn9808[0x5F8];
		/* +0x9E00 */ uint8 restoreDL[0x300]; // display list with the LOAD_* packets
	};
	static_assert(offsetof(GX2ContextState, restoreDLSize) == 0x9804);
	static_assert(offsetof(GX2ContextState, restoreDL) == 0x9E00);
	static_assert(sizeof(GX2ContextState) == 0xA100);

	MPTR s_activeContextState = MPTR_NULL;

	// CONTEXT_CONTROL: 0x80000077 enables loading and shadowing of all register blocks,
	// 0x80000000 disables shadowing
	void EmitContextControl(uint32 value)
	{
		GX2::GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(pm4HeaderType3(IT_CONTEXT_CONTROL, 2), value, value);
	}

	template<size_t N>
	void EmitLoad(uint32 opcode, void* shadowArea, uint32 waitFlag, const RegRange (&ranges)[N])
	{
		GX2::GX2ReserveCmdSpace(3 + (uint32)N * 2);
		gx2WriteGather_submit(pm4HeaderType3(opcode, 2 + (uint32)N * 2),
			memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(shadowArea)), waitFlag);
		for (const RegRange& r : ranges)
			gx2WriteGather_submit(r.offset, r.count);
	}

	// enable shadowing and reload every register block from the state's shadow memory
	void EmitRestoreState(GX2ContextState* state)
	{
		EmitContextControl(0x80000077);
		EmitLoad(IT_LOAD_CONFIG_REG, state->shadow.config, 0x80000000, kConfigRanges); // wait for idle first
		EmitLoad(IT_LOAD_CONTEXT_REG, state->shadow.context, 0, kContextRanges);
		EmitLoad(IT_LOAD_ALU_CONST, state->shadow.aluConst, 0, kAluConstRanges);
		EmitLoad(IT_LOAD_LOOP_CONST, state->shadow.loopConst, 0, kLoopConstRanges);
		EmitLoad(IT_LOAD_RESOURCE, state->shadow.resource, 0, kResourceRanges);
		EmitLoad(IT_LOAD_SAMPLER, state->shadow.sampler, 0, kSamplerRanges);
	}

	// records EmitRestoreState() into the display list stored in the context state
	void BuildRestoreDisplayList(GX2ContextState* state)
	{
		cemu_assert_debug(!GX2::GX2GetDisplayListWriteStatus());
		GX2::GX2BeginDisplayList(state->restoreDL, sizeof(state->restoreDL));
		EmitRestoreState(state);
		state->restoreDLSize = GX2::GX2EndDisplayList(state->restoreDL);
	}
}

// the GX2 power-on render state
static void GX2SetDefaultState()
{
	using MODE_CNTL = Latte::LATTE_PA_SU_SC_MODE_CNTL;
	GX2::GX2ReserveCmdSpace(0x100);

	GX2::GX2SetShaderModeEx(GX2::GX2_SHADER_MODE::UNIFORM_REGISTER, 48, 64, 0, 0, 200, 192);

	// regular viewport transform
	Latte::LATTE_PA_CL_VTE_CNTL vte{};
	vte.set_VPORT_X_SCALE_ENA(true).set_VPORT_X_OFFSET_ENA(true);
	vte.set_VPORT_Y_SCALE_ENA(true).set_VPORT_Y_OFFSET_ENA(true);
	vte.set_VPORT_Z_SCALE_ENA(true).set_VPORT_Z_OFFSET_ENA(true);
	vte.set_VTX_W0_FMT(true);
	gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), Latte::REGADDR::PA_CL_VTE_CNTL - 0xA000, vte);

	// depth test and write on, compare LESS, stencil off
	Latte::LATTE_DB_DEPTH_CONTROL depthControl{};
	depthControl.set_Z_ENABLE(true).set_Z_WRITE_ENABLE(true).set_Z_FUNC(Latte::LATTE_DB_DEPTH_CONTROL::E_ZFUNC::LESS);
	gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), Latte::REGADDR::DB_DEPTH_CONTROL - 0xA000, depthControl);

	GX2::GX2SetAlphaTest(GX2_DISABLE, GX2::GX2_ALPHAFUNC::LESS, 0.0f);
	GX2::GX2SetPolygonControl(MODE_CNTL::E_FRONTFACE::CCW, GX2_DISABLE, GX2_DISABLE, MODE_CNTL::E_POLYGONMODE::UKN0,
		MODE_CNTL::E_PTYPE::TRIANGLES, MODE_CNTL::E_PTYPE::TRIANGLES, GX2_DISABLE, GX2_DISABLE, GX2_DISABLE);
	GX2::GX2SetPolygonOffset(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
	GX2::GX2SetPrimitiveRestartIndex(0xFFFFFFFF);
	GX2::GX2SetTargetChannelMasks(0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF);
	GX2::GX2SetBlendConstantColor(0.0f, 0.0f, 0.0f, 0.0f);
	GX2::GX2SetPointSize(1.0f, 1.0f);
	GX2::GX2SetPointLimits(1.0f, 1.0f);
	GX2::GX2SetColorControl(GX2::GX2_LOGICOP::COPY, GX2_DISABLE, GX2_DISABLE, GX2_ENABLE);
	GX2::GX2SetRasterizerClipControlEx(true, true, false);

	// clear depth 1.0 (Darksiders 2 clears without setting it)
	gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), mmDB_DEPTH_CLEAR - 0xA000, 1.0f);

	// turn off all HLE special states (ids 0..GX2_SPECIAL_STATE_COUNT)
	for (uint32 id = 0; id <= GX2_SPECIAL_STATE_COUNT; id++)
		gx2WriteGather_submit(pm4HeaderType3(IT_HLE_SPECIAL_STATE, 2), id, 0);
}

void gx2Export_GX2SetDefaultState(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetDefaultState()");
	GX2SetDefaultState();
	osLib_returnFromFunction(hCPU, 0);
}

// r3 = context state, r4 = enable profiling
void gx2Export_GX2SetupContextStateEx(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetupContextStateEx(0x{:08x})", hCPU->gpr[3]);
	cemu_assert_debug(hCPU->gpr[4] <= 1);
	GX2ContextState* state = (GX2ContextState*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
	memset(state, 0, sizeof(GX2ContextState));
	state->profilingEnabled = _swapEndianU32(hCPU->gpr[4] & 1);

	// make the new state current (its shadow memory is still all zero), then put the default
	// render state into it and finally prebuild the restore display list
	EmitRestoreState(state);
	s_activeContextState = hCPU->gpr[3];
	EmitContextControl(0x80000077);
	GX2SetDefaultState();
	BuildRestoreDisplayList(state);

	osLib_returnFromFunction(hCPU, 0);
}

// r3 = context state or null (null turns state shadowing off)
void gx2Export_GX2SetContextState(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetContextState(0x{:08x})", hCPU->gpr[3]);
	const MPTR stateAddr = hCPU->gpr[3];
	if (stateAddr == MPTR_NULL)
	{
		EmitContextControl(0x80000000);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}
	GX2ContextState* state = (GX2ContextState*)memory_getPointerFromVirtualOffset(stateAddr);
	// The Binding of Isaac flickers if the already active state gets reloaded. For it the state
	// is restored directly and only when it changes.
	const bool skipSameStateReload = CafeSystem::GetRPXHashBase() == 0x6BCD618E;
	if (skipSameStateReload)
	{
		if (stateAddr != s_activeContextState)
		{
			EmitRestoreState(state);
			EmitContextControl(0x80000077);
			s_activeContextState = stateAddr;
		}
		else
		{
			EmitContextControl(0x80000077); // keep shadowing enabled
		}
	}
	else
	{
		EmitContextControl(0x80000077);
		if (state->restoreDLSize == 0)
			EmitRestoreState(state);
		else
			GX2::GX2CallDisplayList(memory_getVirtualOffsetFromPointer(state->restoreDL), state->restoreDLSize);
		s_activeContextState = stateAddr;
	}
	osLib_returnFromFunction(hCPU, 0);
}

// r3 = context state, r4 = display list address out, r5 = display list size out
void gx2Export_GX2GetContextStateDisplayList(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2GetContextStateDisplayList(0x{:08x}, 0x{:08x}, 0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
	ppcDefineParamStructPtr(state, GX2ContextState, 0);
	ppcDefineParamU32BEPtr(displayListPtrOut, 1);
	ppcDefineParamU32BEPtr(displayListSizeOut, 2);
	*displayListPtrOut = memory_getVirtualOffsetFromPointer(state->restoreDL);
	*displayListSizeOut = state->restoreDLSize;
	osLib_returnFromFunction(hCPU, 0);
}
