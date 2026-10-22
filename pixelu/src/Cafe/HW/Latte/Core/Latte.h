// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "util/VirtualHeap/VirtualHeap.h"

// Central GPU state and the entry points of the Latte core modules.

struct LatteTextureDefinition;
class LatteTexture;
class LatteTextureView;

// lives in guest memory, read and written by GX2 on the CPU side (big endian)
struct gx2GPUSharedArea_t
{
	volatile uint32 flipRequestCountBE;
	volatile uint32 flipExecuteCountBE;
	volatile uint32 swapInterval; // 0: no vsync
};

struct LatteGPUState_t
{
	// GPU registers, raw or through the typed register view
	union
	{
		uint32 contextRegister[LATTE_MAX_REGISTER];
		LatteContextRegister contextNew;
	};
	MPTR contextRegisterShadowAddr[LATTE_MAX_REGISTER];
	uint32 contextControl0;
	uint32 contextControl1;
	// use the scissor box to guess the unpadded render target size
	bool allowFramebufferSizeOptimization{false};
	// counters
	uint32 frameCounter;
	uint32 flipCounter; // vsync flips
	uint32 currentDrawCallTick; // time at the start of the current draw
	uint32 drawCallCounter;
	uint32 textureBindCounter; // increased per texture binding pass
	std::atomic<uint64> flipRequestCount;
	// vsync timing
	uint64 timer_frequency;
	uint64 timer_bootUp; // GPU timer start
	uint64 timer_nextVSync;
	// GX2 shared area
	gx2GPUSharedArea_t* sharedArea;
	MPTR sharedAreaAddr;
	uint32 gx2InitCalled; // number of GX2Init calls
	uint32 glVendor; // GLVENDOR_*
	// the main window shows the GamePad image (toggled with Ctrl+Tab)
	bool isDRCPrimary = false;
	// scan buffer color space and gamma
	bool tvBufferUsesSRGB;
	bool drcBufferUsesSRGB;
	float tvGamma = 0.0f;
	float drcGamma = 0.0f;
	// draw state
	bool activeShaderHasError; // a bound shader stage failed to translate, draws are skipped
	bool repeatTextureInitialization; // set when setup invalidated textures, the setup has to run again
	bool requiresTextureBarrier;
	// OSScreen framebuffers (TV and GamePad)
	struct
	{
		struct
		{
			bool isEnabled;
			MPTR physPtr;
			std::atomic<uint32> flipRequestCount;
			std::atomic<uint32> flipExecuteCount;
		}screen[2];
	}osScreen;
};

extern LatteGPUState_t LatteGPUState;

// what changed since the previous draw of a sequence of draws
struct LatteDrawcallContext
{
	bool isFirst{}; // the masks below are only meaningful when this is false
	mutable uint32 vertexBufferDirtyMask{};
	uint32 vsUniformBufferDirtyMask{}; // uniform buffers with a new address or size
	uint32 psUniformBufferDirtyMask{};
	uint32 gsUniformBufferDirtyMask{};
	bool aluConstVSDirty{};
	bool aluConstPSDirty{};
};

#include "Cafe/HW/Latte/Core/LatteTexture.h"

// texture loader
void LatteTextureLoader_estimateAccessedDataRange(LatteTexture* texture, sint32 sliceIndex, sint32 mipIndex, uint32& addrStart, uint32& addrEnd);

// render targets (LatteRenderTarget.cpp)
#define RENDER_TARGET_TV (1 << 0)
#define RENDER_TARGET_DRC (1 << 2)

void LatteRenderTarget_updateScissorBox();
void LatteRenderTarget_updateViewport();
void LatteRenderTarget_trackUpdates();
void LatteRenderTarget_getScreenImageArea(sint32* x, sint32* y, sint32* width, sint32* height, sint32* fullWidth, sint32* fullHeight, bool padView = false);
void LatteRenderTarget_copyToBackbuffer(LatteTextureView* textureView, bool isPadView);
void LatteRenderTarget_GetCurrentVirtualViewportSize(sint32* viewportWidth, sint32* viewportHeight);
void LatteRenderTarget_itHLESwapScanBuffer();
void LatteRenderTarget_itHLEClearColorDepthStencil(uint32 clearMask, MPTR colorBufferMPTR, Latte::E_GX2SURFFMT colorBufferFormat, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferPitch, uint32 colorBufferViewFirstSlice, uint32 colorBufferViewNumSlice, MPTR depthBufferMPTR, Latte::E_GX2SURFFMT depthBufferFormat, Latte::E_HWTILEMODE depthBufferTileMode, sint32 depthBufferWidth, sint32 depthBufferHeight, sint32 depthBufferPitch, sint32 depthBufferViewFirstSlice, sint32 depthBufferViewNumSlice, float r, float g, float b, float a, float clearDepth, uint32 clearStencil);
void LatteRenderTarget_itHLECopyColorBufferToScanBuffer(MPTR colorBufferPtr, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferSliceIndex, uint32 colorBufferFormat, uint32 colorBufferPitch, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferSwizzle, uint32 renderTarget);
void LatteRenderTarget_unloadAll();

// texture cache
void LatteTC_Init();
void LatteTC_RegisterTexture(LatteTexture* tex);
void LatteTC_UnregisterTexture(LatteTexture* tex);
uint32 LatteTexture_CalculateTextureDataHash(LatteTexture* hostTexture);
void LatteTexture_ReloadData(LatteTexture* hostTexture);
bool LatteTC_HasTextureChanged(LatteTexture* hostTexture, bool force = false);
void LatteTC_ResetTextureChangeTracker(LatteTexture* hostTexture, bool force = false);
void LatteTC_FlagSliceAsGPUUpdated(LatteTexture* hostTexture, uint32 sliceIndex, uint32 mipIndex);
// keeps the texture from being collected as unused
void LatteTC_MarkTextureStillInUse(LatteTexture* texture);
void LatteTC_CleanupUnusedTextures();
std::vector<LatteTexture*> LatteTC_GetDeleteableTextures();
void LatteTC_UnloadAllTextures();

// texture readback to guest memory
void LatteTextureReadback_Initate(LatteTextureView* textureView);
void LatteTextureReadback_StartTransfer(LatteTextureView* textureView);
bool LatteTextureReadback_Update(bool forceStart = false);
void LatteTextureReadback_NotifyTextureDeletion(LatteTexture* texture);
void LatteTextureReadback_UpdateFinishedTransfers(bool forceFinish);
bool LatteTextureReadback_ReadbackToLinearBlocking(LatteTextureView* sourceView, uint8* dstPtr, uint32 dstWidth, uint32 dstHeight, uint32 dstPitch);

// occlusion queries
void LatteQuery_Init();
void LatteQuery_BeginOcclusionQuery(MPTR queryMPTR);
void LatteQuery_EndOcclusionQuery(MPTR queryMPTR);
void LatteQuery_UpdateFinishedQueries();
void LatteQuery_UpdateFinishedQueriesForceFinishAll();
void LatteQuery_CancelActiveGPU7Queries();

// streamout
void LatteStreamout_InitCache();
sint32 LatteStreamout_GetRingBufferSize();
void LatteStreamout_PrepareDrawcall(uint32 count, uint32 instanceCount);
void LatteStreamout_FinishDrawcall(bool useDirectMemoryMode);

// vsync timing
void LatteTiming_Init();
void LatteTiming_HandleTimedVsync();

// command processor
void LatteCP_ProcessRingbuffer();

// buffer cache
void LatteBufferCache_Sync(uint32 maxVtxIndex, uint32 baseInstance, uint32 instanceCount, uint32 attribBufferDirtyMask, uint32 vsUniformBufferDirtyMask, uint32 psUniformBufferDirtyMask, uint32 gsUniformBufferDirtyMask, uint8& stageUniformModifiedMask, bool isIncremental = false);
bool LatteBufferCache_LoadRemappedUniforms(struct LatteDecompilerShader* shader, float* uniformData, bool aluConstDirty, uint32 uniformBufferDirtyMask);

// largest dynamic uniform block, in vec4 (64KB, the Nvidia limit)
#define LATTE_GLSL_DYNAMIC_UNIFORM_BLOCK_SIZE	(4096)

// placeholder for OpenGL error checks, compiled out
#define catchOpenGLError()

// GPU thread control (LatteThread.cpp)
void Latte_Start();
void Latte_Stop();
bool Latte_GetStopSignal(); // true once a stop was requested
void LatteThread_Exit();
