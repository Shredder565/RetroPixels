// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/CafeSystem.h"
#include "input/InputManager.h"

#include "GX2.h"
#include "GX2_Command.h"
#include "GX2_Memory.h"
#include "GX2_Resource.h"
#include "GX2_Surface.h"
#include "GX2_Surface_Copy.h"
#include "GX2_Texture.h"
#include "GX2_State.h"
#include "GX2_Shader.h"
#include "GX2_Event.h"
#include "GX2_Blit.h"
#include "GX2_Draw.h"
#include "GX2_Streamout.h"
#include "GX2_Query.h"
#include "GX2_Misc.h"

// Scan buffers (the images that end up on the TV and the GamePad), presenting and
// the gx2.rpl module itself.
//
// Presenting does not copy anything here: the PM4 stream carries HLE packets that
// Latte's command processor turns into the actual copy and flip. PixelU's GamePad
// screen toggle and the recorder hook in at that end, so the packets below have to
// stay exactly as they are.

// time of the most recent GX2SwapScanBuffers in OSTime ticks (read by GX2GetSwapStatus)
uint64 lastSwapTime = 0;

namespace
{
	// TV render modes as GX2CalcTVSize gets them
	enum TVRenderMode : uint32
	{
		TV_MODE_NONE = 0,
		TV_MODE_480 = 1,
		TV_MODE_480_WIDE = 2,
		TV_MODE_720 = 3,
		TV_MODE_720I = 4,
		TV_MODE_1080 = 5,
		TV_MODE_COUNT
	};

	struct ScanSize
	{
		uint32 width;
		uint32 height;
	};

	constexpr ScanSize kTVScanSize[TV_MODE_COUNT] =
	{
		{ 0, 0 },       // NONE
		{ 640, 480 },   // 480
		{ 854, 480 },   // 480 wide
		{ 1280, 720 },  // 720
		{ 1280, 720 },  // 720 interlaced
		{ 1920, 1080 }, // 1080
	};

	constexpr ScanSize kDRCScanSize = { 854, 480 };

	// titles that need the GPU to be idle before every swap
	bool SwapNeedsDrawDone(uint64 titleId)
	{
		switch (titleId)
		{
		case 0x00050000101DF500ull: // Pokken Tournament
		case 0x00050000101C5800ull:
		case 0x00050000101DF400ull:
			return true;
		default:
			return false;
		}
	}

	uint32 FlipRequestCount()
	{
		return _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE);
	}

	uint32 FlipExecuteCount()
	{
		return _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE);
	}

	// the CPU may run at most this many swaps ahead of the GPU
	constexpr sint32 kMaxFlipsInFlight = 5;

	// a linear, single slice 2D colour buffer of the given size, sized like the console does it
	void DescribeScanBuffer(GX2ColorBuffer& cb, uint32 width, uint32 height, Latte::E_GX2SURFFMT format)
	{
		GX2Surface& s = cb.surface;
		s.resFlag = GX2_RESFLAG_USAGE_TEXTURE | GX2_RESFLAG_USAGE_COLOR_BUFFER;
		s.width = width;
		s.height = height;
		s.depth = 1;
		s.numLevels = 1;
		s.dim = Latte::E_DIM::DIM_2D;
		s.format = format;
		s.tileMode = Latte::E_GX2TILEMODE::TM_LINEAR_GENERAL;
		s.swizzle = 0;
		s.aa = 0;
		s.mipPtr = MPTR_NULL;
		cb.viewMip = 0;
		cb.viewFirstSlice = 0;
		cb.viewNumSlices = 1;
		GX2::GX2CalcSurfaceSizeAndAlignment(&s);
		// the scan buffer flag is only added once the size is known
		s.resFlag = GX2_RESFLAG_USAGE_TEXTURE | GX2_RESFLAG_USAGE_COLOR_BUFFER | GX2_RESFLAG_USAGE_SCAN_BUFFER;
	}

	// bytes needed for 'bufferCount' scan buffers that each start aligned; the last one needs no padding after it
	// 'fieldCount' is 2 for interlaced output, which needs room for both fields
	uint32 ScanBufferSetSize(uint32 width, uint32 height, Latte::E_GX2SURFFMT format, uint32 bufferCount, uint32 fieldCount)
	{
		GX2ColorBuffer cb;
		memset(&cb, 0, sizeof(cb));
		DescribeScanBuffer(cb, width, height, format);
		const uint32 imageSize = cb.surface.imageSize;
		const uint32 alignment = cb.surface.alignment;
		const uint32 padding = (alignment - imageSize % alignment) % alignment;
		const uint32 stride = imageSize + padding;
		return stride * bufferCount * fieldCount - padding;
	}
}

void gx2Export_GX2SwapScanBuffers(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SwapScanBuffers()");

	if (SwapNeedsDrawDone(CafeSystem::GetForegroundTitleId()))
		GX2::GX2DrawDone();

	// two 2-word packets (plus headroom so the pair is never split)
	GX2::GX2ReserveCmdSpace(7);

	lastSwapTime = PPCInterpreter_getMainCoreCycleCounter() / 20ULL;

	// Some titles (Orochi Warriors) present from other cores. Their packets still reach the GPU, but only
	// the core that owns the command buffer counts the request, otherwise the wait below would never end.
	if ((uint32)coreinit::OSGetCoreId() == GX2::sGX2MainCoreIndex)
		LatteGPUState.sharedArea->flipRequestCountBE = _swapEndianU32(FlipRequestCount() + 1);

	gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_REQUEST_SWAP_BUFFERS, 1));
	gx2WriteGather_submitU32AsBE(0);
	gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_TRIGGER_SCANBUFFER_SWAP, 1));
	gx2WriteGather_submitU32AsBE(0);

	// throttle the CPU when it gets too far ahead (done after queuing the swap, like the console)
	while ((sint32)(FlipRequestCount() - FlipExecuteCount()) > kMaxFlipsInFlight)
		GX2::GX2WaitForFlip();

	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2CopyColorBufferToScanBuffer(PPCInterpreter_t* hCPU)
{
	const MPTR colorBufferAddr = hCPU->gpr[3];
	const uint32 scanTarget = hCPU->gpr[4];
	cemuLog_log(LogType::GX2, "GX2CopyColorBufferToScanBuffer(0x{:08x},{})", colorBufferAddr, scanTarget);

	GX2::GX2ReserveCmdSpace(10);
	const GX2ColorBuffer* cb = (const GX2ColorBuffer*)memory_getPointerFromVirtualOffset(colorBufferAddr);
	const GX2Surface& s = cb->surface;
	// header + 9 words: where the image is, how it is laid out, and which screen it is meant for
	gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER, 9));
	gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(s.imagePtr));
	gx2WriteGather_submitU32AsBE((uint32)s.width);
	gx2WriteGather_submitU32AsBE((uint32)s.height);
	gx2WriteGather_submitU32AsBE((uint32)s.pitch);
	gx2WriteGather_submitU32AsBE((uint32)s.tileMode.value());
	gx2WriteGather_submitU32AsBE((uint32)s.swizzle);
	gx2WriteGather_submitU32AsBE((uint32)cb->viewFirstSlice);
	gx2WriteGather_submitU32AsBE((uint32)s.format.value());
	gx2WriteGather_submitU32AsBE(scanTarget);

	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2WaitForFreeScanBuffer(PPCInterpreter_t* hCPU)
{
	// scan buffers are never busy from the guest's point of view
	debug_printf("GX2WaitForFreeScanBuffer(): Unimplemented\n");
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2GetCurrentScanBuffer(PPCInterpreter_t* hCPU)
{
	const uint32 scanTarget = hCPU->gpr[3];
	GX2ColorBuffer* cb = (GX2ColorBuffer*)memory_getPointerFromVirtualOffset(hCPU->gpr[4]);
	// there is no real scan buffer memory; hand out a small placeholder that points into the tiling aperture range
	memset(cb, 0, sizeof(GX2ColorBuffer));
	cb->surface.width = 100;
	cb->surface.height = 100;
	switch (scanTarget)
	{
	case GX2_SCAN_TARGET_TV:
		cb->surface.imagePtr = MEMORY_TILINGAPERTURE_AREA_ADDR + 0x200000;
		break;
	case GX2_SCAN_TARGET_DRC_FIRST:
		cb->surface.imagePtr = MEMORY_TILINGAPERTURE_AREA_ADDR + 0x40000;
		break;
	default:
		break;
	}
	osLib_returnFromFunction(hCPU, 0);
}

static void gx2Export_GX2GetSystemTVScanMode(PPCInterpreter_t* hCPU)
{
	osLib_returnFromFunction(hCPU, 7); // 1080p
}

static void gx2Export_GX2GetSystemTVAspectRatio(PPCInterpreter_t* hCPU)
{
	osLib_returnFromFunction(hCPU, 1); // 16:9
}

void gx2Export_GX2TempGetGPUVersion(PPCInterpreter_t* hCPU)
{
	osLib_returnFromFunction(hCPU, 2);
}

void gx2Export_GX2CalcTVSize(PPCInterpreter_t* hCPU)
{
	const uint32 mode = hCPU->gpr[3];
	const auto format = (Latte::E_GX2SURFFMT)hCPU->gpr[4];
	const uint32 bufferingMode = hCPU->gpr[5];
	const MPTR sizeOut = hCPU->gpr[6];
	const MPTR scaleNeededOut = hCPU->gpr[7];

	cemu_assert(mode < TV_MODE_COUNT);
	if (mode >= TV_MODE_COUNT)
	{
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	// the SD modes always reserve four buffers
	const uint32 bufferCount = mode < TV_MODE_720 ? 4 : bufferingMode;
	const uint32 fieldCount = mode == TV_MODE_720I ? 2 : 1;
	const uint32 size = ScanBufferSetSize(kTVScanSize[mode].width, kTVScanSize[mode].height, format, bufferCount, fieldCount);

	memory_writeU32(sizeOut, size);
	memory_writeU32(scaleNeededOut, 0);
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2CalcDRCSize(PPCInterpreter_t* hCPU)
{
	ppcDefineParamS32(drcMode, 0);
	ppcDefineParamU32(format, 1);
	ppcDefineParamU32(bufferingMode, 2);
	ppcDefineParamMPTR(sizeOut, 3);
	ppcDefineParamMPTR(scaleNeededOut, 4);

	// mode 0 means no GamePad output, which still reports the (empty) buffer size
	const ScanSize dim = drcMode > 0 ? kDRCScanSize : ScanSize{ 0, 0 };
	const uint32 size = ScanBufferSetSize(dim.width, dim.height, (Latte::E_GX2SURFFMT)format, bufferingMode, 1);

	memory_writeU32(sizeOut, size);
	memory_writeU32(scaleNeededOut, 0);
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2SetDRCScale(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetDRCScale({},{})", hCPU->gpr[3], hCPU->gpr[4]);
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2SetDRCConnectCallback(PPCInterpreter_t* hCPU)
{
	ppcDefineParamS32(channel, 0);
	ppcDefineParamMEMPTR(callback, void, 1);
	cemuLog_log(LogType::GX2, "GX2SetDRCConnectCallback({}, 0x{:08x})", channel, callback.GetMPTR());
	if (callback.GetPtr())
	{
		// The first GamePad is always there. A second one only counts when it is configured,
		// some titles (Just Dance 2018) stop taking input if it is reported without being usable.
		const bool connected = channel == 0 || InputManager::instance().get_vpad_controller(channel) != nullptr;
		PPCCoreCallback(callback, channel, connected ? 1 : 0);
	}
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2SetSemaphore(PPCInterpreter_t* hCPU)
{
	ppcDefineParamMPTR(semaphoreAddr, 0);
	ppcDefineParamS32(mode, 1);
	cemuLog_log(LogType::GX2, "GX2SetSemaphore(0x{:08x},{})", hCPU->gpr[3], hCPU->gpr[4]);

	// MEM_SEMAPHORE control word: operation in bits 29-31, bit 12 makes a wait block until signalled
	constexpr uint32 kSemOpSignal = 6;
	constexpr uint32 kSemOpWait = 7;
	constexpr uint32 kWaitOnSignal = 0x1000;

	uint32 op;
	switch (mode)
	{
	case 0: op = kSemOpWait; break;
	case 1: op = kSemOpSignal; break;
	default:
		cemu_assert_debug(false);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	GX2::GX2ReserveCmdSpace(3);
	gx2WriteGather_submitU32AsBE(pm4HeaderType3(IT_MEM_SEMAPHORE, 2));
	gx2WriteGather_submitU32AsBE(memory_virtualToPhysical(semaphoreAddr));
	gx2WriteGather_submitU32AsBE((op << 29) | kWaitOnSignal);

	osLib_returnFromFunction(hCPU, 0);
}

namespace GX2
{
	namespace
	{
		struct LegacyExport
		{
			const char* name;
			void (*fn)(PPCInterpreter_t*);
		};

		// registration order is kept stable
		const LegacyExport kLegacyExports[] =
		{
			{ "GX2GetContextStateDisplayList", gx2Export_GX2GetContextStateDisplayList },

			// presenting
			{ "GX2SwapScanBuffers", gx2Export_GX2SwapScanBuffers },
			{ "GX2GetSwapStatus", gx2Export_GX2GetSwapStatus },
			{ "GX2CopyColorBufferToScanBuffer", gx2Export_GX2CopyColorBufferToScanBuffer },
			{ "GX2WaitForFreeScanBuffer", gx2Export_GX2WaitForFreeScanBuffer },
			{ "GX2GetCurrentScanBuffer", gx2Export_GX2GetCurrentScanBuffer },

			// shaders and uniform blocks
			{ "GX2SetPixelShader", gx2Export_GX2SetPixelShader },
			{ "GX2SetGeometryShader", gx2Export_GX2SetGeometryShader },
			{ "GX2SetComputeShader", gx2Export_GX2SetComputeShader },
			{ "GX2SetVertexUniformBlock", gx2Export_GX2SetVertexUniformBlock },
			{ "GX2RSetVertexUniformBlock", gx2Export_GX2RSetVertexUniformBlock },
			{ "GX2SetPixelUniformBlock", gx2Export_GX2SetPixelUniformBlock },
			{ "GX2SetGeometryUniformBlock", gx2Export_GX2SetGeometryUniformBlock },
			{ "GX2CalcGeometryShaderInputRingBufferSize", gx2Export_GX2CalcGeometryShaderInputRingBufferSize },
			{ "GX2CalcGeometryShaderOutputRingBufferSize", gx2Export_GX2CalcGeometryShaderOutputRingBufferSize },

			// render targets
			{ "GX2InitColorBufferRegs", gx2Export_GX2InitColorBufferRegs },
			{ "GX2InitDepthBufferRegs", gx2Export_GX2InitDepthBufferRegs },
			{ "GX2SetColorBuffer", gx2Export_GX2SetColorBuffer },
			{ "GX2SetDepthBuffer", gx2Export_GX2SetDepthBuffer },
			{ "GX2SetDRCBuffer", gx2Export_GX2SetDRCBuffer },
			{ "GX2MarkScanBufferCopied", gx2Export_GX2MarkScanBufferCopied },

			// display configuration
			{ "GX2TempGetGPUVersion", gx2Export_GX2TempGetGPUVersion },
			{ "GX2CalcTVSize", gx2Export_GX2CalcTVSize },
			{ "GX2CalcDRCSize", gx2Export_GX2CalcDRCSize },
			{ "GX2SetDRCScale", gx2Export_GX2SetDRCScale },
			{ "GX2SetDRCConnectCallback", gx2Export_GX2SetDRCConnectCallback },
			{ "GX2GetSystemTVScanMode", gx2Export_GX2GetSystemTVScanMode },
			{ "GX2GetSystemTVAspectRatio", gx2Export_GX2GetSystemTVAspectRatio },

			// timing
			{ "GX2SetSwapInterval", gx2Export_GX2SetSwapInterval },
			{ "GX2GetSwapInterval", gx2Export_GX2GetSwapInterval },
			{ "GX2GetGPUTimeout", gx2Export_GX2GetGPUTimeout },
			{ "GX2SampleTopGPUCycle", gx2Export_GX2SampleTopGPUCycle },
			{ "GX2SampleBottomGPUCycle", gx2Export_GX2SampleBottomGPUCycle },

			// tiling aperture
			{ "GX2AllocateTilingApertureEx", gx2Export_GX2AllocateTilingApertureEx },
			{ "GX2FreeTilingAperture", gx2Export_GX2FreeTilingAperture },

			// context state
			{ "GX2SetDefaultState", gx2Export_GX2SetDefaultState },
			{ "GX2SetupContextStateEx", gx2Export_GX2SetupContextStateEx },
			{ "GX2SetContextState", gx2Export_GX2SetContextState },

			// synchronisation
			{ "GX2SetSemaphore", gx2Export_GX2SetSemaphore },
		};

		class GX2Module : public COSModule
		{
		public:
			std::string_view GetName() override
			{
				return "gx2";
			}

			std::vector<std::string_view> GetDependencies() override
			{
				return { "avm", "coreinit", "tcl" };
			}

			void RPLMapped() override
			{
				for (const LegacyExport& e : kLegacyExports)
					osLib_addFunction("gx2", e.name, e.fn);

				// each part registers its own typed exports; the order matters for init side effects
				GX2MemInit();
				GX2ResourceInit();
				GX2CommandInit();
				GX2SurfaceInit();
				GX2SurfaceCopyInit();
				GX2TextureInit();
				GX2StateInit();
				GX2ShaderInit();
				GX2EventInit();
				GX2BlitInit();
				GX2DrawInit();
				GX2StreamoutInit();
				GX2QueryInit();
				GX2MiscInit();
			}
		};

		GX2Module s_module;
	}

	COSModule* GetModule()
	{
		return &s_module;
	}
}
