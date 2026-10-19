// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"

#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"

#include "GX2_Command.h"

// Color and depth buffers: GX2Init*BufferRegs precompute the CB_COLORn_* / DB_DEPTH_* register
// values inside the buffer structure, GX2Set*Buffer emits them together with the base address.
// All exports here use the register level calling convention and return 0.

namespace
{
	// CB_COLORn_SIZE / DB_DEPTH_SIZE: pitch in 8 pixel units and slice size in 64 pixel units, both minus one
	uint32 MakeSizeReg(uint32 pitch, uint32 height)
	{
		const uint32 pitchField = ((pitch >> 3) - 1) & 0x3FF;
		const uint32 sliceField = (((height * pitch) >> 6) - 1) & 0xFFFFF;
		return pitchField | (sliceField << 10);
	}

	// CB_COLORn_VIEW / DB_DEPTH_VIEW: first and last slice
	uint32 MakeSliceViewReg(uint32 firstSlice, uint32 numSlices)
	{
		return (firstSlice & 0x7FF) | (((numSlices + firstSlice - 1) & 0x7FF) << 13);
	}

	// CB_COLORn_INFO number type (bits 12-14) for the format class bits 8-11 of a GX2 surface format
	bool ColorNumberType(uint32 formatClass, uint32& numberType, bool& clampBlend)
	{
		switch (formatClass)
		{
		case 0x000: numberType = 0; clampBlend = true; return true; // unorm
		case 0x100: numberType = 4; clampBlend = false; return true; // uint
		case 0x200: numberType = 1; clampBlend = true; return true; // snorm
		case 0x300: numberType = 5; clampBlend = false; return true; // sint
		case 0x400: numberType = 6; clampBlend = true; return true; // srgb
		case 0x800: numberType = 7; clampBlend = false; return true; // float
		default: return false;
		}
	}

	uint32 MakeColorInfoReg(const GX2ColorBuffer* cb, Latte::E_HWTILEMODE hwTileMode)
	{
		using FMT = Latte::E_GX2SURFFMT;
		const FMT format = cb->surface.format;
		const Latte::E_HWFMT hwFormat = Latte::GetHWFormat(format);
		uint32 info = (uint32)GX2::GetSurfaceFormatSwapMode(format); // ENDIAN, bits 0-1
		info |= (uint32)hwFormat << 2; // FORMAT
		cemu_assert_debug(LatteAddrLib::IsValidHWTileMode(hwTileMode));
		info |= (uint32)hwTileMode << 8; // ARRAY_MODE

		uint32 numberType = 0;
		bool clampBlend = false;
		if (ColorNumberType((uint32)format & 0xF00, numberType, clampBlend))
			info |= numberType << 12;
		else
			cemu_assert_debug(false);

		// COMP_SWAP (bits 16-17): alternate component order for 5_5_5_1 and 10_10_10_2
		if (hwFormat == Latte::E_HWFMT::HWFMT_5_5_5_1 || hwFormat == Latte::E_HWFMT::HWFMT_10_10_10_2)
			info |= 2 << 16;
		else
			info &= ~(3u << 16);
		if (cb->surface.aa != 0)
			info |= 2 << 18; // multisampled tiling

		const bool isInteger = HAS_FLAG(format, FMT::FMT_BIT_INT);
		if (!isInteger)
			info |= GX2::GetSurfaceColorBufferExportFormat(format) << 27; // SOURCE_FORMAT
		// formats without blending support bypass the blender
		if (isInteger || format == FMT::R24_X8_UNORM || format == FMT::R24_X8_FLOAT || format == FMT::R32_X8_FLOAT)
		{
			info |= 1 << 22; // BLEND_BYPASS
			clampBlend = false;
		}
		if (clampBlend)
			info |= 1 << 20; // BLEND_CLAMP
		if (HAS_FLAG(format, FMT::FMT_BIT_FLOAT))
			info |= 1 << 25; // ROUND_MODE
		return info;
	}

	// emits one SET_CONTEXT_REG for a single register (3 words)
	void EmitContextReg(uint32 reg, uint32 value)
	{
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), reg - 0xA000, value);
	}
}

void gx2Export_GX2InitColorBufferRegs(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2InitColorBufferRegs(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamStructPtr(colorBuffer, GX2ColorBuffer, 0);
	const GX2Surface& s = colorBuffer->surface;
	// note: the surface usage flags are not passed on to addrlib here
	LatteAddrLib::AddrSurfaceInfo_OUT info{};
	LatteAddrLib::GX2CalculateSurfaceInfo(s.format, s.width, s.height, s.depth, s.dim, s.tileMode, s.aa, colorBuffer->viewMip, &info);

	colorBuffer->reg_size = MakeSizeReg(info.pitch, info.height);
	colorBuffer->reg_info = MakeColorInfoReg(colorBuffer, info.hwTileMode);
	// the special linear mode has no slices
	colorBuffer->reg_view = s.tileMode != Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL ? MakeSliceViewReg(colorBuffer->viewFirstSlice, colorBuffer->viewNumSlices) : 0;
	colorBuffer->reg_mask = 0;
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2InitDepthBufferRegs(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2InitDepthBufferRegs(0x{:08x})", hCPU->gpr[3]);
	ppcDefineParamStructPtr(depthBuffer, GX2DepthBuffer, 0);
	const GX2Surface& s = depthBuffer->surface;
	LatteAddrLib::AddrSurfaceInfo_OUT info{};
	LatteAddrLib::GX2CalculateSurfaceInfo(s.format, s.width, s.height, s.depth, s.dim, s.tileMode, s.aa, depthBuffer->viewMip, &info);
	cemu_assert_debug(depthBuffer->viewNumSlices != 0);
	// only the size register is needed by the GPU emulation
	depthBuffer->reg_size = MakeSizeReg(info.pitch, info.height);
	osLib_returnFromFunction(hCPU, 0);
}

// r3 = GX2ColorBuffer*, r4 = render target index
void gx2Export_GX2SetColorBuffer(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetColorBuffer(0x{:08x}, {})", hCPU->gpr[3], hCPU->gpr[4]);
	GX2::GX2ReserveCmdSpace(20);
	const MPTR cbAddr = hCPU->gpr[3];
	GX2ColorBuffer* cb = (GX2ColorBuffer*)memory_getPointerFromVirtualOffset(cbAddr);
	const uint32 target = hCPU->gpr[4];

	// address of the viewed mip level (mipOffset[n] holds the offset of level n+1 relative to mipPtr)
	const uint32 viewMip = cb->viewMip;
	MPTR levelAddr = cb->surface.imagePtr;
	if (viewMip == 1)
		levelAddr = cb->surface.mipPtr;
	else if (viewMip > 1)
		levelAddr = (uint32)cb->surface.mipPtr + memory_readU32(cbAddr + (uint32)offsetof(GX2ColorBuffer, surface.mipOffset) + (viewMip - 1) * 4);
	uint32 base = memory_virtualToPhysical(levelAddr);
	// levels that are still macro tiled carry the bank/pipe swizzle in their address
	const uint32 swizzle = cb->surface.swizzle;
	if (Latte::TM_IsMacroTiled(cb->surface.tileMode) && viewMip < ((swizzle >> 16) & 0xFF))
		base ^= swizzle & 0xFFFF;
	cemu_assert_debug(cb->surface.tileMode != Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL);

	EmitContextReg(mmCB_COLOR0_BASE + target, base);
	EmitContextReg(mmCB_COLOR0_SIZE + target, cb->reg_size);
	EmitContextReg(mmCB_COLOR0_VIEW + target, cb->reg_view);
	EmitContextReg(mmCB_COLOR0_INFO + target, cb->reg_info);
	osLib_returnFromFunction(hCPU, 0);
}

// r3 = GX2DepthBuffer*
void gx2Export_GX2SetDepthBuffer(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2SetDepthBuffer(0x{:08x})", hCPU->gpr[3]);
	GX2::GX2ReserveCmdSpace(20);
	GX2DepthBuffer* db = (GX2DepthBuffer*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
	const GX2Surface& s = db->surface;
	cemuLog_log(LogType::GX2, "DepthBuffer tileMode {:01x} PhysAddr {:08x} fmt {:04x} res {}x{}", (uint32)s.tileMode.value(), (uint32)s.imagePtr, (uint32)s.format.value(), (uint32)s.width, (uint32)s.height);

	// The GPU emulation finds the depth buffer through DB_HTILE_DATA_BASE (the image address);
	// DB_DEPTH_BASE stays zero. Only mip 0 is supported.
	const uint32 imageBase = memory_virtualToPhysical(s.imagePtr) >> 8;
	if (db->viewMip > 0)
		cemuLog_logDebug(LogType::Force, "GX2SetDepthBuffer: Unsupported non-zero mip ({}) Pointer: {:08x} Base: {:08x}", (uint32)db->viewMip, imageBase, 0);

	// DB_DEPTH_INFO: format in bits 0-2, tile mode in bits 15-18
	uint32 depthInfo = ((uint32)s.tileMode.value() & 0xF) << 15;
	switch (s.format.value())
	{
	case Latte::E_GX2SURFFMT::D16_UNORM: depthInfo |= 1; break;
	case Latte::E_GX2SURFFMT::D24_S8_UNORM: depthInfo |= 3; break;
	case Latte::E_GX2SURFFMT::D24_S8_FLOAT: depthInfo |= 5; break;
	case Latte::E_GX2SURFFMT::D32_FLOAT: depthInfo |= 6; break;
	case Latte::E_GX2SURFFMT::D32_S8_FLOAT: depthInfo |= 7; break;
	default:
		debug_printf("GX2SetDepthBuffer(): unsupported depth format 0x%04x\n", (uint32)s.format.value());
		break;
	}

	EmitContextReg(mmDB_DEPTH_SIZE, db->reg_size);
	gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 4), mmDB_DEPTH_BASE - 0xA000,
		0, depthInfo, imageBase); // DB_DEPTH_BASE, DB_DEPTH_INFO, DB_HTILE_DATA_BASE
	EmitContextReg(mmDB_DEPTH_VIEW, MakeSliceViewReg(db->viewFirstSlice, db->viewNumSlices));
	osLib_returnFromFunction(hCPU, 0);
}

// r6 = surface format; only the sRGB flag is needed
void gx2Export_GX2SetDRCBuffer(PPCInterpreter_t* hCPU)
{
	LatteGPUState.drcBufferUsesSRGB = HAS_FLAG((Latte::E_GX2SURFFMT)hCPU->gpr[6], Latte::E_GX2SURFFMT::FMT_BIT_SRGB);
	osLib_returnFromFunction(hCPU, 0);
}

// r3 = scan target. For the TV the scan buffer image placed in the tiling aperture area
// (1920x1080 RGBA, 2D tiled) is presented.
void gx2Export_GX2MarkScanBufferCopied(PPCInterpreter_t* hCPU)
{
	const uint32 scanTarget = hCPU->gpr[3];
	if (scanTarget == GX2_SCAN_TARGET_TV)
	{
		GX2::GX2ReserveCmdSpace(10);
		gx2WriteGather_submit(pm4HeaderType3(IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER, 9),
			MEMORY_TILINGAPERTURE_AREA_ADDR + 0x200000, // physical address
			1920, 1080, // width, height
			1920, // pitch
			4, // tile mode
			0, // swizzle
			0,
			(uint32)Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM,
			scanTarget);
	}
	osLib_returnFromFunction(hCPU, 0);
}
