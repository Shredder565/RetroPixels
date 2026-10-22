// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteAsyncCommands.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "GX2.h"
#include "GX2_Surface_Copy.h"

/* CPU side texel copy between two (possibly tiled) surfaces, used by Latte for copies in RAM */

namespace
{
	// one side of a software copy: where the texels are and how they are laid out
	struct CopySide
	{
		uint8* base;
		sint32 height;
		sint32 pitch;
		sint32 depth;
		uint32 slice;
		uint32 pipeSwizzle;
		uint32 bankSwizzle;
		uint32 hwTileMode;

		CopySide(uint8* data, sint32 h, sint32 p, sint32 d, uint32 s, uint32 swizzle, uint32 tileMode)
			: base(data), height(h), pitch(p), depth(d), slice(s),
			  pipeSwizzle((swizzle >> 8) & 1), bankSwizzle((swizzle >> 9) & 3),
			  hwTileMode(tileMode == 16 ? 0 : tileMode) // LINEAR_SPECIAL is laid out like LINEAR_GENERAL
		{
		}

		// byte offset of texel (x,y) in this slice
		uint32 Offset(uint32 x, uint32 y, uint32 bpp) const
		{
			switch (hwTileMode)
			{
			case 0:
			case 1:
				return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, slice, 0, bpp, pitch, height, depth);
			case 2:
			case 3:
				return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, slice, bpp, pitch, height, (Latte::E_HWTILEMODE)hwTileMode, false);
			default:
				return MacroOffset(x, y, bpp, (Latte::E_HWTILEMODE)hwTileMode);
			}
		}

		uint32 MacroOffset(uint32 x, uint32 y, uint32 bpp, Latte::E_HWTILEMODE tm) const
		{
			return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, slice, 0, bpp, pitch, height, 1, tm, false, pipeSwizzle, bankSwizzle);
		}

		// two sides share every tile address when these match
		bool SameMacroLayout(const CopySide& o) const
		{
			return slice == o.slice && pipeSwizzle == o.pipeSwizzle && bankSwizzle == o.bankSwizzle && height == o.height && pitch == o.pitch;
		}
	};

	// Texels wider than 32 bits move as consecutive 32-bit words, so the result is the same as
	// copying word by word even when source and destination overlap.
	template<uint32 TBytes>
	void MoveTexel(uint8* dst, const uint8* src)
	{
		if constexpr (TBytes == 1)
		{
			dst[0] = src[0];
		}
		else if constexpr (TBytes == 2)
		{
			uint16 v;
			memcpy(&v, src, 2);
			memcpy(dst, &v, 2);
		}
		else
		{
			for (uint32 w = 0; w < TBytes / 4; w++)
			{
				uint32 v;
				memcpy(&v, src + w * 4, 4);
				memcpy(dst + w * 4, &v, 4);
			}
		}
	}

	template<uint32 TBytes>
	void CopyTexels(const CopySide& src, const CopySide& dst, uint32 width, uint32 height)
	{
		constexpr uint32 bpp = TBytes * 8;
		for (uint32 y = 0; y < height; y++)
		{
			for (uint32 x = 0; x < width; x++)
			{
				const uint8* in = src.base + src.Offset(x, y, bpp);
				uint8* out = dst.base + dst.Offset(x, y, bpp);
				MoveTexel<TBytes>(out, in);
			}
		}
	}

	// 2D_TILED_THIN1 on both sides with whole 8x8 micro tiles: with at most 32 bits per texel a micro tile
	// is stored contiguously (64 texels <= 256 bytes), so each one can be moved in a single block
	void CopyMicroTiles(const CopySide& src, const CopySide& dst, uint32 width, uint32 height, uint32 bpp)
	{
		const uint32 tileBytes = (bpp / 8) * 64;
		const bool shared = src.SameMacroLayout(dst);
		for (uint32 y = 0; y < height; y += 8)
		{
			for (uint32 x = 0; x < width; x += 8)
			{
				const uint32 inOffset = src.MacroOffset(x, y, bpp, Latte::E_HWTILEMODE::TM_2D_TILED_THIN1);
				const uint32 outOffset = shared ? inOffset : dst.MacroOffset(x, y, bpp, Latte::E_HWTILEMODE::TM_2D_TILED_THIN1);
				memmove(dst.base + outOffset, src.base + inOffset, tileBytes); // source and destination may overlap
			}
		}
	}
}

void gx2SurfaceCopySoftware(
	uint8* inputData, sint32 surfSrcHeight, sint32 srcPitch, sint32 srcDepth, uint32 srcSlice, uint32 srcSwizzle, uint32 srcHwTileMode,
	uint8* outputData, sint32 surfDstHeight, sint32 dstPitch, sint32 dstDepth, uint32 dstSlice, uint32 dstSwizzle, uint32 dstHwTileMode,
	uint32 copyWidth, uint32 copyHeight, uint32 copyBpp)
{
	const CopySide src(inputData, surfSrcHeight, srcPitch, srcDepth, srcSlice, srcSwizzle, srcHwTileMode);
	const CopySide dst(outputData, surfDstHeight, dstPitch, dstDepth, dstSlice, dstSwizzle, dstHwTileMode);

	const bool wholeMicroTiles = (copyWidth % 8) == 0 && (copyHeight % 8) == 0;
	if (src.hwTileMode == 4 && dst.hwTileMode == 4 && wholeMicroTiles && copyBpp <= 32)
	{
		CopyMicroTiles(src, dst, copyWidth, copyHeight, copyBpp);
		return;
	}

	switch (copyBpp)
	{
	case 8: CopyTexels<1>(src, dst, copyWidth, copyHeight); break;
	case 16: CopyTexels<2>(src, dst, copyWidth, copyHeight); break;
	case 32: CopyTexels<4>(src, dst, copyWidth, copyHeight); break;
	case 64: CopyTexels<8>(src, dst, copyWidth, copyHeight); break;
	case 128: CopyTexels<16>(src, dst, copyWidth, copyHeight); break;
	default:
		cemu_assert_debug(false);
		break;
	}
}

/*
 * GX2CopySurface and friends.
 *
 * On the console a copy involving a LINEAR_SPECIAL surface is done by the CPU before the call returns,
 * every other copy is drawn by the GPU in command order. Here the texture data may only exist on the
 * host GPU, so both kinds go to the render thread:
 *  - CPU copies are queued as an async command and waited for
 *  - GPU copies become an HLE packet in the command stream
 */

namespace
{
	struct GX2Rect
	{
		sint32be left;
		sint32be top;
		sint32be right;
		sint32be bottom;
	};
	static_assert(sizeof(GX2Rect) == 0x10);

	struct GX2Point
	{
		sint32be x;
		sint32be y;
	};
	static_assert(sizeof(GX2Point) == 0x8);

	// size of a mip level along one axis; the shift count wraps like the x86 shift instruction does
	sint32 MipShift(GX2Surface* s, uint32be GX2Surface::* dim, uint32 mip)
	{
		return (sint32)(uint32)(s->*dim) >> (mip & 31);
	}

	// signed: used to check that both sides describe the same level
	sint32 MipExtent(GX2Surface* s, uint32be GX2Surface::* dim, uint32 mip)
	{
		return std::max<sint32>(MipShift(s, dim, mip), 1);
	}

	// unsigned: the copied size (differs from the signed one only for sizes of 2^31 and above)
	uint32 MipCopyExtent(GX2Surface* s, uint32be GX2Surface::* dim, uint32 mip)
	{
		return std::max<uint32>((uint32)MipShift(s, dim, mip), 1);
	}

	// guest address of the first texel of a mip level
	MPTR MipLevelAddress(GX2Surface* surface, uint32 mip)
	{
		if (mip == 0)
			return surface->imagePtr;
		if (mip == 1)
			return surface->mipPtr;
		// mipOffset[i] is relative to mipPtr and describes level i+1. The index is read through guest
		// memory so that a level count beyond the array reads the same bytes as the console would.
		const MPTR offsetsAddr = memory_getVirtualOffsetFromPointer(surface) + (uint32)offsetof(GX2Surface, mipOffset);
		return (uint32)surface->mipPtr + memory_readU32(offsetsAddr + (mip - 1) * 4);
	}

	void LogSurface(const char* label, GX2Surface* s, uint32 mip, uint32 slice)
	{
		cemuLog_log(LogType::GX2, "{}: imagePtr=0x{:08x} mipPtr=0x{:08x} swizzle=0x{:06x} width={} height={} depth={} pitch=0x{:x} tilemode=0x{:x} format=0x{:x} mip={} slice={}",
			label, (uint32)s->imagePtr, (uint32)s->mipPtr, (uint32)s->swizzle, (uint32)s->width, (uint32)s->height, (uint32)s->depth, (uint32)s->pitch,
			(uint32)s->tileMode.value(), (uint32)s->format.value(), mip, slice);
	}

	// tile mode the copy works with: LINEAR_SPECIAL is kept as is, everything else is what addrlib picked
	Latte::E_GX2TILEMODE CopyTileMode(GX2Surface* s, const LatteAddrLib::AddrSurfaceInfo_OUT& info)
	{
		if (s->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL)
			return s->tileMode.value();
		return (Latte::E_GX2TILEMODE)info.hwTileMode;
	}

	LatteSurfaceCopyParam MakeCopyParam(GX2Surface* s, MPTR data, const LatteAddrLib::AddrSurfaceInfo_OUT& info, Latte::E_GX2TILEMODE tileMode, uint32 slice)
	{
		LatteSurfaceCopyParam p{};
		p.physDataAddr = data;
		p.swizzle = s->swizzle;
		p.surfaceFormat = s->format;
		p.heightInTexels = info.height;
		p.pitch = info.pitch;
		p.dim = s->dim;
		p.tilemode = tileMode;
		p.aa = s->aa;
		p.sliceIndex = (sint32)slice;
		return p;
	}

	void CopySurfaceLevel(GX2Surface* srcSurface, uint32 srcMip, uint32 srcSlice, GX2Surface* dstSurface, uint32 dstMip, uint32 dstSlice)
	{
		if (MipExtent(dstSurface, &GX2Surface::width, dstMip) != MipExtent(srcSurface, &GX2Surface::width, srcMip) ||
			MipExtent(dstSurface, &GX2Surface::height, dstMip) != MipExtent(srcSurface, &GX2Surface::height, srcMip))
		{
			cemuLog_logDebugOnce(LogType::Force, "GX2CopySurface: Mismatching mip resolution");
			return;
		}

		LatteAddrLib::AddrSurfaceInfo_OUT srcInfo = {};
		LatteAddrLib::AddrSurfaceInfo_OUT dstInfo = {};
		GX2::GX2CalculateSurfaceInfo(srcSurface, srcMip, &srcInfo);
		GX2::GX2CalculateSurfaceInfo(dstSurface, dstMip, &dstInfo);

		if (srcSurface->numLevels == 0)
		{
			debug_printf("GX2CopySurface(): mip count is 0\n");
			return;
		}
		// texels are copied as raw bits, so only the size per texel has to agree
		if (srcInfo.bpp != dstInfo.bpp)
		{
			cemuLog_logDebug(LogType::Force, "GX2CopySurface(): Format bpp mismatch (src=0x{:04x} dst=0x{:04x})", (sint32)srcSurface->format.value(), (sint32)dstSurface->format.value());
			return;
		}

		cemu_assert(srcMip < srcSurface->numLevels);
		const MPTR srcData = MipLevelAddress(srcSurface, srcMip);
		cemu_assert(dstMip < dstSurface->numLevels);
		const MPTR dstData = MipLevelAddress(dstSurface, dstMip);

		// the pitch comes from addrlib, it can differ from the one stored in the surface
		const Latte::E_GX2TILEMODE srcTileMode = CopyTileMode(srcSurface, srcInfo);
		const Latte::E_GX2TILEMODE dstTileMode = CopyTileMode(dstSurface, dstInfo);

		cemuLog_log(LogType::GX2, "GX2CopySurface:");
		LogSurface("srcSurface", srcSurface, srcMip, srcSlice);
		LogSurface("dstSurface", dstSurface, dstMip, dstSlice);

		const uint32 width = MipCopyExtent(dstSurface, &GX2Surface::width, dstMip);
		const uint32 height = MipCopyExtent(dstSurface, &GX2Surface::height, dstMip);

		const bool cpuCopy = srcSurface->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL || dstSurface->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL;
		if (cpuCopy)
		{
			LatteSurfaceCopyRect rect{};
			rect.x = 0;
			rect.y = 0;
			rect.width = width;
			rect.height = height;
			LatteAsyncCommand_queueTextureCopy(MakeCopyParam(srcSurface, srcData, srcInfo, srcTileMode, srcSlice),
				MakeCopyParam(dstSurface, dstData, dstInfo, dstTileMode, dstSlice), rect);
			// the guest expects the data to be there once the call returns
			LatteAsyncCommands_waitUntilAllProcessed();
			return;
		}

		// HLE packet: rect (x, y, width, height), then 9 words for the source and 9 for the destination
		GX2::GX2ReserveCmdSpace(23);
		gx2WriteGather_submit(pm4HeaderType3(IT_HLE_COPY_SURFACE_NEW, 4 + 9 * 2),
			(uint32)0, (uint32)0, width, height,
			(uint32)srcData, (uint32)srcSurface->swizzle, (uint32)srcSurface->format.value(), (uint32)srcInfo.pitch, (uint32)srcInfo.height,
			srcSlice, (uint32)srcSurface->dim.value(), (uint32)srcTileMode, (uint32)srcSurface->aa,
			(uint32)dstData, (uint32)dstSurface->swizzle, (uint32)dstSurface->format.value(), (uint32)dstInfo.pitch, (uint32)dstInfo.height,
			dstSlice, (uint32)dstSurface->dim.value(), (uint32)dstTileMode, (uint32)dstSurface->aa);
	}
}

void gx2Export_GX2CopySurface(PPCInterpreter_t* hCPU)
{
	GX2Surface* src = (GX2Surface*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
	GX2Surface* dst = (GX2Surface*)memory_getPointerFromVirtualOffset(hCPU->gpr[6]);
	CopySurfaceLevel(src, hCPU->gpr[4], hCPU->gpr[5], dst, hCPU->gpr[7], hCPU->gpr[8]);
	osLib_returnFromFunction(hCPU, 0);
}

// Only the case where a single rect covers the whole source is supported; it behaves like GX2CopySurface.
// Partial rects are ignored.
void gx2Export_GX2CopySurfaceEx(PPCInterpreter_t* hCPU)
{
	const MPTR srcAddr = hCPU->gpr[3];
	const uint32 srcMip = hCPU->gpr[4];
	const uint32 srcSlice = hCPU->gpr[5];
	const MPTR dstAddr = hCPU->gpr[6];
	const uint32 dstMip = hCPU->gpr[7];
	const uint32 dstSlice = hCPU->gpr[8];
	const sint32 rectCount = (sint32)hCPU->gpr[9];
	const MPTR rectsAddr = hCPU->gpr[10];
	const MPTR pointsAddr = memory_readU32(hCPU->gpr[1] + 8); // 9th argument is on the stack
	cemuLog_logDebug(LogType::Force, "GX2CopySurfaceEx(0x{:08x},{},{},0x{:08x},{},{},{},0x{:08x},0x{:08x})", srcAddr, srcMip, srcSlice, dstAddr, dstMip, dstSlice, rectCount, rectsAddr, pointsAddr);

	GX2Surface* src = (GX2Surface*)memory_getPointerFromVirtualOffset(srcAddr);
	GX2Surface* dst = (GX2Surface*)memory_getPointerFromVirtualOffset(dstAddr);
	const GX2Rect* rects = (const GX2Rect*)memory_getPointerFromVirtualOffset(rectsAddr);

	cemu_assert_debug(rectCount == 1 && srcMip == 0 && srcSlice == 0 && dstMip == 0 && dstSlice == 0);

	for (sint32 i = 0; i < rectCount; i++)
	{
		const GX2Rect& r = rects[i];
		cemuLog_logDebug(LogType::Force, "rect left-top: {}/{} right-bottom: {}/{}", (sint32)r.left, (sint32)r.top, (sint32)r.right, (sint32)r.bottom);
		const bool coversSource = r.left == 0 && r.top == 0 && (uint32)(sint32)r.right == (uint32)src->width && (uint32)(sint32)r.bottom == (uint32)src->height;
		if (coversSource)
			CopySurfaceLevel(src, srcMip, srcSlice, dst, dstMip, dstSlice);
	}

	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2ResolveAAColorBuffer(PPCInterpreter_t* hCPU)
{
	debug_printf("GX2ResolveAAColorBuffer(0x%08x,0x%08x,%d,%d)\n", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6]);
	GX2ColorBuffer* colorBuffer = (GX2ColorBuffer*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
	GX2Surface* src = &colorBuffer->surface;
	GX2Surface* dst = (GX2Surface*)memory_getPointerFromVirtualOffset(hCPU->gpr[4]);
	const uint32 srcMip = colorBuffer->viewMip;
	const uint32 srcSlice = colorBuffer->viewFirstSlice;
	const uint32 dstMip = hCPU->gpr[5];
	const uint32 dstSlice = hCPU->gpr[6];
	cemu_assert_debug(srcMip == 0 && srcSlice == 0);

	// the resolve is a plain copy of sample 0, so source and target have to describe the same image
	if (src->width != dst->width || src->height != dst->height)
	{
		osLib_returnFromFunction(hCPU, 0);
		return;
	}
	if (MipExtent(dst, &GX2Surface::width, dstMip) != MipExtent(src, &GX2Surface::width, srcMip) ||
		MipExtent(dst, &GX2Surface::height, dstMip) != MipExtent(src, &GX2Surface::height, srcMip))
	{
		cemu_assert_suspicious();
		osLib_returnFromFunction(hCPU, 0);
		return;
	}
	const Latte::E_HWFMT srcFormat = Latte::GetHWFormat(src->format);
	const Latte::E_HWFMT dstFormat = Latte::GetHWFormat(dst->format);
	if (srcFormat != dstFormat)
	{
		debug_printf("GX2ResolveAAColorBuffer(): Format mismatch\n");
		cemu_assert_unimplemented();
		osLib_returnFromFunction(hCPU, 0);
		return;
	}
	CopySurfaceLevel(src, srcMip, srcSlice, dst, dstMip, dstSlice);
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2ConvertDepthBufferToTextureSurface(PPCInterpreter_t* hCPU)
{
	cemuLog_log(LogType::GX2, "GX2ConvertDepthBufferToTextureSurface(0x{:x}, 0x{:x}, {}, {})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5], hCPU->gpr[6]);
	GX2DepthBuffer* depthBuffer = (GX2DepthBuffer*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
	GX2Surface* dst = (GX2Surface*)memory_getPointerFromVirtualOffset(hCPU->gpr[4]);
	const uint32 dstMip = hCPU->gpr[5];
	const uint32 dstSlice = hCPU->gpr[6];

	// same memory: the depth data is already where the texture expects it, nothing to move
	if (depthBuffer->surface.imagePtr == dst->imagePtr)
	{
		debug_printf("In-place re-tiling\n");
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	// one copy per viewed slice, at least one
	const uint32 sliceCount = std::max<uint32>(depthBuffer->viewNumSlices, 1);
	for (uint32 i = 0; i < sliceCount; i++)
		CopySurfaceLevel(&depthBuffer->surface, depthBuffer->viewMip, depthBuffer->viewFirstSlice + i, dst, dstMip, dstSlice + i);

	osLib_returnFromFunction(hCPU, 0);
}

namespace GX2
{
	void GX2SurfaceCopyInit()
	{
		osLib_addFunction("gx2", "GX2CopySurface", gx2Export_GX2CopySurface);
		osLib_addFunction("gx2", "GX2CopySurfaceEx", gx2Export_GX2CopySurfaceEx);
		osLib_addFunction("gx2", "GX2ResolveAAColorBuffer", gx2Export_GX2ResolveAAColorBuffer);
		osLib_addFunction("gx2", "GX2ConvertDepthBufferToTextureSurface", gx2Export_GX2ConvertDepthBufferToTextureSurface);
	}
};

// startup hook for timing the software copy; nothing to do in normal builds
void gx2CopySurfaceTest()
{
}
