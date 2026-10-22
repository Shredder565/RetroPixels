// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "WindowSystem.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "util/helpers/helpers.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"

// The GPU thread: sets up the renderer and the GPU-side caches, waits for a title to
// start, loads the shader cache and then runs the command processor until it is
// told to stop.

LatteGPUState_t LatteGPUState = {};

void LatteThread_HandleOSScreen();

namespace
{
	std::atomic_bool s_running = false; // cleared to ask the GPU thread to stop
	std::atomic_bool s_initDone = false;
	std::thread s_gpuThread;
	std::mutex s_startStopMutex;

	// register values the GPU has after reset
	void LoadResetRegisters()
	{
		auto& ctx = LatteGPUState.contextNew;
		ctx.CB_TARGET_MASK.set_MASK(0xFFFFFFFF);
		ctx.VGT_MULTI_PRIM_IB_RESET_INDX.set_RESTART_INDEX(0xFFFFFFFF);
		ctx.VGT_DMA_NUM_INSTANCES.set_NUM_INSTANCES(1);
		LatteGPUState.contextRegister[Latte::REGADDR::PA_CL_CLIP_CNTL] = 0;
		const float clearDepth = 1.0f;
		memcpy(&LatteGPUState.contextRegister[mmDB_DEPTH_CLEAR], &clearDepth, sizeof(float));
	}

	// OSScreen (the system's simple framebuffer API) keeps two buffers per screen in guest memory
	struct OSScreenLayout
	{
		uint32 width;
		uint32 height;
		uint32 pitch;
	};
	constexpr OSScreenLayout kOSScreenLayout[2] = {
		{ 1280, 720, 1280 }, // TV
		{ 854, 480, 896 }, // GamePad
	};

	LatteTextureView* OSScreenBuffer(uint32 screen, uint32 bufferIndex)
	{
		const OSScreenLayout& layout = kOSScreenLayout[screen];
		const MPTR addr = LatteGPUState.osScreen.screen[screen].physPtr + bufferIndex * layout.pitch * layout.height * 4;
		const auto format = Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM;
		LatteTextureView* view = LatteTextureViewLookupCache::lookup(addr, layout.width, layout.height, 1, layout.pitch, 0, 1, 0, 1, format, Latte::E_DIM::DIM_2D);
		if (view)
			return view;
		return LatteTexture_CreateTexture(Latte::E_DIM::DIM_2D, addr, 0, format, layout.width, layout.height, 1, layout.pitch, 1, 0, Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED, false);
	}

	// shows the buffer that was flipped in last, returns false if there was nothing new
	bool PresentOSScreen(uint32 screen)
	{
		auto& state = LatteGPUState.osScreen.screen[screen];
		if (!state.isEnabled)
			return false;
		const uint32 requested = state.flipRequestCount;
		if (state.flipExecuteCount == requested)
			return false;
		// all four buffer textures are looked up (or created) each time, like the guest expects them to exist
		LatteTextureView* buffers[2][2];
		for (uint32 s = 0; s < 2; s++)
			for (uint32 b = 0; b < 2; b++)
				buffers[s][b] = OSScreenBuffer(s, b);
		LatteTexture* front = buffers[screen][(requested & 1) ^ 1]->baseTexture;
		LatteTexture_ReloadData(front);
		LatteRenderTarget_copyToBackbuffer(front->baseView, screen == 1);
		state.flipExecuteCount.store(state.flipRequestCount);
		return true;
	}

	uint32 VendorId(GfxVendor vendor)
	{
		switch (vendor)
		{
		case GfxVendor::AMD: return GLVENDOR_AMD;
		case GfxVendor::Intel: return GLVENDOR_INTEL;
		case GfxVendor::Nvidia: return GLVENDOR_NVIDIA;
		case GfxVendor::Apple: return GLVENDOR_APPLE;
		default: return GLVENDOR_UNKNOWN;
		}
	}

	// legacy graphic packs that resize textures need the full padded render target size
	bool GraphicPacksAllowSizeOptimization()
	{
		for (auto& pack : GraphicPack2::GetActiveGraphicPacks())
		{
			if (pack->AllowRendertargetSizeOptimization())
				continue;
			for (auto& rule : pack->GetTextureRules())
			{
				const bool resizes = rule.filter_settings.width >= 0 || rule.filter_settings.height >= 0 || rule.filter_settings.depth >= 0 ||
					rule.overwrite_settings.width >= 0 || rule.overwrite_settings.height >= 0 || rule.overwrite_settings.depth >= 0;
				if (resizes)
				{
					cemuLog_log(LogType::Force, "Graphic pack \"{}\" prevents rendertarget size optimization. This warning can be ignored and is intended for graphic pack developers", pack->GetName());
					return false;
				}
			}
		}
		return true;
	}

	void GPUThreadMain()
	{
		SetThreadName("LatteThread");
		sint32 windowWidth, windowHeight;
		WindowSystem::GetWindowPhysSize(windowWidth, windowHeight);

		g_renderer->Initialize();
		RendererOutputShader::InitializeStatic();
		LatteTiming_Init();
		LatteTexture_init();
		LatteTC_Init();
		LatteBufferCache_init(164 * 1024 * 1024);
		LatteQuery_Init();
		LatteSHRC_Init();
		LatteStreamout_InitCache();
		g_renderer->renderTarget_setViewport(0, 0, windowWidth, windowHeight, 0.0f, 1.0f);
		LatteGPUState.glVendor = VendorId(g_renderer->GetVendor());
		s_initDone = true;

		if (cemuLog_isLoggingEnabled(LogType::OpenGLLogging))
			g_renderer->EnableDebugMode();

		// idle frames until a title runs
		while (!CafeSystem::IsTitleRunning())
		{
			g_renderer->DrawEmptyFrame(true);
			g_renderer->DrawEmptyFrame(false);
			g_renderer->CancelScreenshotRequest();
			std::this_thread::sleep_for(std::chrono::milliseconds(1000 / 60));
		}
		g_renderer->DrawEmptyFrame(true);

		// the graphic packs must be loaded before any game shader is created
		GraphicPack2::WaitUntilReady();
		LatteGPUState.allowFramebufferSizeOptimization = GraphicPacksAllowSizeOptimization();
		LatteShaderCache_Load();
		LoadResetRegisters();
		g_isGPUInitFinished = true;

		// OSScreen output can arrive before the game initializes GX2
		while (LatteGPUState.gx2InitCalled == 0)
		{
			std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			LatteThread_HandleOSScreen();
			if (Latte_GetStopSignal())
				LatteThread_Exit();
		}
		LatteCP_ProcessRingbuffer(); // leaves through LatteThread_Exit
		cemu_assert_debug(false);
	}
}

void LatteThread_HandleOSScreen()
{
	const bool tv = PresentOSScreen(0);
	const bool drc = PresentOSScreen(1);
	if (tv || drc)
		g_renderer->SwapBuffers(tv, drc);
}

// starts the GPU thread and returns once it has set up the renderer
void Latte_Start()
{
	std::unique_lock lock(s_startStopMutex);
	cemu_assert_debug(!s_running);
	s_running = true;
	s_initDone = false;
	s_gpuThread = std::thread(GPUThreadMain);
	while (!s_initDone)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void Latte_Stop()
{
	std::unique_lock lock(s_startStopMutex);
	if (!s_running)
		return;
	s_running = false;
	lock.unlock();
	s_gpuThread.join();
}

bool Latte_GetStopSignal()
{
	return !s_running;
}

// tears down everything the GPU thread owns and ends the thread
void LatteThread_Exit()
{
	if (g_renderer)
		g_renderer->Shutdown();
	LatteBufferCache_UnloadAll();
	LatteTC_UnloadAllTextures();
	LatteSHRC_UnloadAll();
	LatteShaderCache_Close();
	RendererOutputShader::ShutdownStatic();
	if (g_renderer)
	{
		// g_renderer has to stay valid while the destructor runs
		delete g_renderer.get();
		g_renderer.release();
	}
	memset(&LatteGPUState, 0, sizeof(LatteGPUState));
#if BOOST_OS_WINDOWS
	ExitThread(0);
#else
	pthread_exit(nullptr);
#endif
}
