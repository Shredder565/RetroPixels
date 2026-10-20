// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/common/OSCommon.h"
#include "GX2.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "config/ActiveSettings.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "GX2_Command.h"
#include "GX2_Event.h"
#include "GX2_Misc.h"
#include "GX2_Memory.h"
#include "GX2_Texture.h"

#include "Cafe/HW/Latte/ISA/LatteReg.h"

// GX2Init/GX2Shutdown, swap interval and timing queries, cache invalidation and a few
// display related setters.

/* swap interval / timing (register level exports, r3 = 0 unless they return a value) */

void gx2Export_GX2SetSwapInterval(PPCInterpreter_t* hCPU)
{
	const uint32 interval = hCPU->gpr[3];
	cemuLog_log(LogType::GX2, "GX2SetSwapInterval({})", interval);
	if (interval < 20)
		LatteGPUState.sharedArea->swapInterval = interval;
	else
		cemuLog_log(LogType::Force, "GX2SetSwapInterval(): ignoring out of range interval {}", interval);
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2GetSwapInterval(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2GetSwapInterval()");
	osLib_returnFromFunction(hCPU, LatteGPUState.sharedArea->swapInterval);
}

extern uint64 lastSwapTime;

// r3: swap count out, r4: flip count out, r5: last flip time out, r6: last vsync time out
void gx2Export_GX2GetSwapStatus(PPCInterpreter_t* hCPU)
{
	memory_writeU32(hCPU->gpr[3], _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE));
	memory_writeU32(hCPU->gpr[4], _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE));
	memory_writeU64(hCPU->gpr[5], lastSwapTime);
	memory_writeU64(hCPU->gpr[6], lastSwapTime);
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2GetGPUTimeout(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2GetGPUTimeout()");
	osLib_returnFromFunction(hCPU, 1000); // milliseconds
}

void gx2Export_GX2SampleTopGPUCycle(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SampleTopGPUCycle(0x{:08x})", hCPU->gpr[3]);
	memory_writeU64(hCPU->gpr[3], coreinit::OSGetSystemTime());
	osLib_returnFromFunction(hCPU, 0);
}

// Bottom-of-pipe GPU timestamps are deliberately never delivered: games that see valid GPU
// times (e.g. Wind Waker HD, Super Mario 3D World) start scaling their render resolution,
// which the texture cache and graphic packs don't cope with. The "invalid" value makes them
// keep a fixed resolution.
void gx2Export_GX2SampleBottomGPUCycle(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SampleBottomGPUCycle(0x{:08x})", hCPU->gpr[3]);
	memory_writeU64(hCPU->gpr[3], 0xFFFFFFFFFFFFFFFFULL);
	osLib_returnFromFunction(hCPU, 0);
}

namespace GX2
{
	SysAllocator<uint8, 640 * 480 * 4, 0x1000> _lastFrame; // backing store for GX2GetLastFrame
	uint32 sGX2MainCoreIndex = 0;

	void _test_AddrLib();

	// GX2Init attribute ids (the argument stream is a list of id/value pairs ending with 0)
	enum class GX2InitAttrib : uint32
	{
		End = 0,
		CommandPoolBase = 1,
		CommandPoolSize = 2,
		Ukn7 = 7,
		Ukn8 = 8,
		Ukn9 = 9,
		Ukn11 = 11,
	};

	struct GX2InitParams
	{
		MEMPTR<void> commandPoolBase{ nullptr };
		uint32 commandPoolSize{ 0 };
	};

	static GX2InitParams _ParseInitAttribs(uint32be* attribs)
	{
		GX2InitParams params;
		if (!attribs)
			return params;
		for (;;)
		{
			const GX2InitAttrib id = (GX2InitAttrib)(uint32)*attribs++;
			switch (id)
			{
			case GX2InitAttrib::End:
				return params;
			case GX2InitAttrib::CommandPoolBase:
				params.commandPoolBase = MEMPTR<void>((uint32)*attribs++);
				break;
			case GX2InitAttrib::CommandPoolSize:
				params.commandPoolSize = *attribs++;
				break;
			case GX2InitAttrib::Ukn7:
			case GX2InitAttrib::Ukn8:
			case GX2InitAttrib::Ukn9:
			case GX2InitAttrib::Ukn11:
				attribs++; // value not needed
				break;
			default:
				// unknown ids have no value word
				cemuLog_log(LogType::Force, "GX2Init: Unsupported init arg {}", (uint32)id);
				break;
			}
		}
	}

	void GX2Init(uint32be* initArgStream)
	{
		if (LatteGPUState.gx2InitCalled)
		{
			cemuLog_logDebug(LogType::Force, "GX2Init() called while already initialized");
			return;
		}
		const GX2InitParams params = _ParseInitAttribs(initArgStream);

		// the calling core becomes the GX2 main core
		const uint32 coreIndex = coreinit::OSGetCoreId();
		cemuLog_log(LogType::GX2, "GX2Init() on core {} by thread 0x{:08x}", coreIndex, MEMPTR<OSThread_t>(coreinit::OSGetCurrentThread()).GetMPTR());
		sGX2MainCoreIndex = coreIndex;

		GX2Init_event();
		GX2Init_commandBufferPool(params.commandPoolBase, params.commandPoolSize);

		// state shared with the GPU emulation (flip counters, swap interval)
		if (LatteGPUState.sharedAreaAddr == MPTR_NULL)
		{
			LatteGPUState.sharedAreaAddr = coreinit_allocFromSysArea(sizeof(gx2GPUSharedArea_t), 0x20);
			LatteGPUState.sharedArea = (gx2GPUSharedArea_t*)memory_getPointerFromVirtualOffset(LatteGPUState.sharedAreaAddr);
		}
		LatteGPUState.sharedArea->flipRequestCountBE = _swapEndianU32(0);
		LatteGPUState.sharedArea->flipExecuteCountBE = _swapEndianU32(0);
		LatteGPUState.sharedArea->swapInterval = 1;

		GX2MEMAllocatorsInit();
		LatteGPUState.gx2InitCalled++; // the GPU thread starts processing from here on
		_test_AddrLib();
	}

	void GX2Shutdown()
	{
		if (!LatteGPUState.gx2InitCalled)
		{
			cemuLog_logDebug(LogType::Force, "GX2Shutdown() called while not initialized");
			return;
		}
		LatteGPUState.gx2InitCalled--;
		if (LatteGPUState.gx2InitCalled != 0)
			return;
		GX2DrawDone();
		GX2Shutdown_commandBufferPool();
		cemuLog_log(LogType::Force, "GX2 shutdown");
	}

	void _GX2DriverReset()
	{
		LatteGPUState.gx2InitCalled = 0;
		sGX2MainCoreIndex = 0;
		GX2CommandResetToDefaultState();
		GX2EventResetToDefaultState();
	}

	sint32 GX2GetMainCoreId(PPCInterpreter_t* hCPU)
	{
		return LatteGPUState.gx2InitCalled != 0 ? (sint32)sGX2MainCoreIndex : -1;
	}

	void GX2ResetGPU(uint32 ukn)
	{
		cemuLog_log(LogType::Force, "GX2ResetGPU()");
		GX2DrawDone();
	}

	void GX2SetTVBuffer(void* imageBuffePtr, uint32 imageBufferSize, E_TVRES tvResolutionMode, uint32 _surfaceFormat, E_TVBUFFERMODE bufferMode)
	{
		// the TV image is produced by the renderer; only the sRGB flag matters here
		LatteGPUState.tvBufferUsesSRGB = HAS_FLAG((Latte::E_GX2SURFFMT)_surfaceFormat, Latte::E_GX2SURFFMT::FMT_BIT_SRGB);
	}

	void GX2SetTVGamma(float gamma)
	{
		LatteGPUState.tvGamma = 1.0f - gamma;
	}

	void GX2SetDRCGamma(float gamma)
	{
		LatteGPUState.drcGamma = 1.0f - gamma;
	}

	// describes a 640x480 RGBA texture for the "last frame" (shown by the HOME menu)
	bool GX2GetLastFrame(uint32 deviceId, GX2Texture* textureOut)
	{
		textureOut->viewFirstMip = 0;
		textureOut->viewFirstSlice = 0;
		textureOut->viewNumMips = 1;
		textureOut->viewNumSlices = 1;
		textureOut->compSel = 0x00010203; // RGBA

		GX2Surface& s = textureOut->surface;
		s.width = 640;
		s.height = 480;
		s.depth = 1;
		s.dim = Latte::E_DIM::DIM_2D;
		s.format = Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM;
		s.tileMode = Latte::E_GX2TILEMODE::TM_LINEAR_ALIGNED;
		s.pitch = 0;
		s.resFlag = 0;
		s.aa = 0;
		GX2CalcSurfaceSizeAndAlignment(&s);
		s.imagePtr = _lastFrame.GetMPTR();

		GX2InitTextureRegs(textureOut);
		return true;
	}

	bool GX2GetLastFrameGammaA(uint32 deviceId, float32be* gamma)
	{
		*gamma = 1.0f;
		return true;
	}

	bool GX2GetLastFrameGammaB(uint32 deviceId, float32be* gamma)
	{
		*gamma = 1.0f;
		return true;
	}

	uint64 GX2GPUTimeToCPUTime(uint64 gpuTime)
	{
		return 0; // GPU times are never valid, see GX2SampleBottomGPUCycle
	}

	uint32 GX2GetSystemDRCMode()
	{
		return 1;
	}

	uint32 GX2IsVideoOutReady()
	{
		return 1;
	}

	// translates GX2 invalidation flags to CP_COHER_CNTL actions and emits a SURFACE_SYNC
	static Latte::E_COHER_CNTL _CoherActionsFor(GX2InvalidationFlag flags)
	{
		using CC = Latte::E_COHER_CNTL;
		CC c{};
		if (HAS_FLAG(flags, GX2InvalidationFlag::GPU_ATTRIB) || HAS_FLAG(flags, GX2InvalidationFlag::GPU_TEXTURE))
			c |= CC::TC_ACTION_ENA;
		if (HAS_FLAG(flags, GX2InvalidationFlag::GPU_UNIFORM_BLOCK))
			c |= CC::TC_ACTION_ENA | CC::SH_ACTION_ENA;
		if (HAS_FLAG(flags, GX2InvalidationFlag::GPU_SHADER))
			c |= CC::SH_ACTION_ENA;
		if (HAS_FLAG(flags, GX2InvalidationFlag::GPU_COLOR_BUFFER))
			c |= CC::CB_ACTION_ENA | CC::CB_ALL_DEST_BASE_ENA;
		if (HAS_FLAG(flags, GX2InvalidationFlag::GPU_DEPTH_BUFFER))
			c |= CC::DB_ACTION_ENA | CC::DB_DEST_BASE_ENA;
		if (HAS_FLAG(flags, GX2InvalidationFlag::GPU_STREAM_OUT))
			c |= CC::SX_ACTION_ENA | CC::SO0_DEST_BASE_ENA | CC::SO1_DEST_BASE_ENA | CC::SO2_DEST_BASE_ENA | CC::SO3_DEST_BASE_ENA;
		if (HAS_FLAG(flags, GX2InvalidationFlag::GPU_EXPORT_BUFFER))
			c |= CC::SX_ACTION_ENA | CC::DB_ACTION_ENA | CC::CB_ACTION_ENA | CC::TC_ACTION_ENA | CC::DEST_BASE_0_ENA;
		return c;
	}

	void GX2Invalidate(GX2InvalidationFlag invalidationFlags, MPTR invalidationAddr, uint32 invalidationSize)
	{
		Latte::E_COHER_CNTL actions = _CoherActionsFor(invalidationFlags);
		// CPU writes: let the buffer cache know the range changed
		if (HAS_FLAG(invalidationFlags, GX2InvalidationFlag::CPU))
			LatteBufferCache_notifyDCFlush(invalidationAddr, invalidationSize);
		if (actions == static_cast<Latte::E_COHER_CNTL>(0))
			return;
		actions |= Latte::E_COHER_CNTL::ENGINE_ME;
		// size and base are in 256 byte units; a size of 0xFFFFFFFF means "everything"
		const uint32 sizeUnits = invalidationSize == 0xFFFFFFFFu ? 0x00FFFFFFu : (invalidationSize + 0xFF) >> 8;
		GX2ReserveCmdSpace(5);
		gx2WriteGather_submit(pm4HeaderType3(IT_SURFACE_SYNC, 4), static_cast<uint32>(actions), sizeUnits,
			memory_virtualToPhysical(invalidationAddr) >> 8,
			4u); // poll interval
	}

	void GX2MiscInit()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("gx2", fn, LogType::GX2)
		EXPORT_FN(GX2Init); EXPORT_FN(GX2Shutdown); EXPORT_FN(GX2GetMainCoreId);
		EXPORT_FN(GX2ResetGPU);

		EXPORT_FN(GX2SetTVBuffer); EXPORT_FN(GX2SetTVGamma); EXPORT_FN(GX2SetDRCGamma);

		EXPORT_FN(GX2GetLastFrame); EXPORT_FN(GX2GetLastFrameGammaA); EXPORT_FN(GX2GetLastFrameGammaB);

		EXPORT_FN(GX2GPUTimeToCPUTime); EXPORT_FN(GX2GetSystemDRCMode); EXPORT_FN(GX2IsVideoOutReady);

		EXPORT_FN(GX2Invalidate);
#undef EXPORT_FN

		sGX2MainCoreIndex = 0;
	}
};
