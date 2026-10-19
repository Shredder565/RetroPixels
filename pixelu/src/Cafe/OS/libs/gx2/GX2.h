// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/OS/RPL/COSModule.h"

// boolean values as the GX2 API passes them
#define GX2_TRUE	1
#define GX2_FALSE	0
#define GX2_ENABLE	1
#define GX2_DISABLE	0

// size of the command ring buffer the GPU reads from (64 MiB)
#define GX2_COMMAND_RING_BUFFER_SIZE	(64*1024*1024)

// scan targets (bit flags) used by the scan buffer functions
#define GX2_SCAN_TARGET_TV				1
#define GX2_SCAN_TARGET_TV_RIGH			2
#define GX2_SCAN_TARGET_DRC_FIRST		4
#define GX2_SCAN_TARGET_DRC_SECOND		8

// number of HLE special states the command processor knows about
#define GX2_SPECIAL_STATE_COUNT			9

#include "GX2_Surface.h"
#include "GX2_Command.h"

namespace GX2
{
	// the gx2.rpl module; mapping it registers every GX2 export
	COSModule* GetModule();
}

// Exports that still use the raw PPC register interface.
// Each one is defined in the file named in the comment above its group.

// GX2_shader_legacy.cpp
void gx2Export_GX2SetPixelShader(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetGeometryShader(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetComputeShader(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetVertexUniformBlock(PPCInterpreter_t* hCPU);
void gx2Export_GX2RSetVertexUniformBlock(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetPixelUniformBlock(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetGeometryUniformBlock(PPCInterpreter_t* hCPU);
void gx2Export_GX2CalcGeometryShaderInputRingBufferSize(PPCInterpreter_t* hCPU);
void gx2Export_GX2CalcGeometryShaderOutputRingBufferSize(PPCInterpreter_t* hCPU);

// GX2_TilingAperture.cpp
void gx2Export_GX2AllocateTilingApertureEx(PPCInterpreter_t* hCPU);
void gx2Export_GX2FreeTilingAperture(PPCInterpreter_t* hCPU);

// GX2_Misc.cpp
void gx2Export_GX2SetSwapInterval(PPCInterpreter_t* hCPU);
void gx2Export_GX2GetSwapInterval(PPCInterpreter_t* hCPU);
void gx2Export_GX2GetSwapStatus(PPCInterpreter_t* hCPU);
void gx2Export_GX2GetGPUTimeout(PPCInterpreter_t* hCPU);
void gx2Export_GX2SampleTopGPUCycle(PPCInterpreter_t* hCPU);
void gx2Export_GX2SampleBottomGPUCycle(PPCInterpreter_t* hCPU);

// GX2_RenderTarget.cpp
void gx2Export_GX2InitColorBufferRegs(PPCInterpreter_t* hCPU);
void gx2Export_GX2InitDepthBufferRegs(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetColorBuffer(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetDepthBuffer(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetDRCBuffer(PPCInterpreter_t* hCPU);
void gx2Export_GX2MarkScanBufferCopied(PPCInterpreter_t* hCPU);

// GX2_ContextState.cpp
void gx2Export_GX2SetDefaultState(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetupContextStateEx(PPCInterpreter_t* hCPU);
void gx2Export_GX2SetContextState(PPCInterpreter_t* hCPU);
void gx2Export_GX2GetContextStateDisplayList(PPCInterpreter_t* hCPU);
