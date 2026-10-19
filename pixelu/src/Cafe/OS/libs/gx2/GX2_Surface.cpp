// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "GX2.h"
#include "GX2_Surface.h"
#include "GX2_Resource.h"

namespace GX2
{
	using Latte::E_GX2TILEMODE;

	void GX2CalculateSurfaceInfo(GX2Surface* surfacePtr, uint32 level, LatteAddrLib::AddrSurfaceInfo_OUT* pSurfOut)
	{
		const uint32 usage = surfacePtr->resFlag;
		LatteAddrLib::GX2CalculateSurfaceInfo(surfacePtr->format, surfacePtr->width, surfacePtr->height, surfacePtr->depth, surfacePtr->dim,
			surfacePtr->tileMode, surfacePtr->aa, level, pSurfOut,
			(usage & GX2_RESFLAG_USAGE_DEPTH_BUFFER) != 0, (usage & GX2_RESFLAG_USAGE_SCAN_BUFFER) != 0);
	}

	// number of significant bits, i.e. how many mip levels a dimension of this size allows
	static uint32 _BitLength(uint32 v)
	{
		uint32 n = 0;
		while (v)
		{
			n++;
			v >>= 1;
		}
		return n;
	}

	static uint32 _MaxLevels(GX2Surface* s)
	{
		if (s->numLevels <= 1)
			return 1;
		uint32 n = std::max(_BitLength(s->width), _BitLength(s->height));
		if (s->dim == Latte::E_DIM::DIM_3D)
			n = std::max(n, _BitLength(s->depth));
		return n;
	}

	static void _ClearSwizzleStopLevel(GX2Surface* s)
	{
		s->swizzle = s->swizzle & 0xFF00FFFF;
	}

	// The FFL (Mii) code embedded in some games (Sonic Lost World, Super Mario 3D World) asks for
	// the size of a texture it never initialised when the Mii data files are missing. Replace such
	// garbage with a small valid surface so the game does not crash further down.
	static void _FixGarbageSurface(GX2Surface* s)
	{
		const bool garbage = (uint32)s->dim.value() >= 50 || (uint32)s->aa >= 0x100 ||
			(uint32)s->width >= 0x01000000 || (uint32)s->height >= 0x01000000 || (uint32)s->depth >= 0x01000000 ||
			(uint32)s->format.value() >= 0x10000;
		if (!garbage)
			return;
		cemuLog_log(LogType::Force, "GX2CalcSurfaceSizeAndAlignment(): surface is not initialised, using a placeholder");
		s->dim = Latte::E_DIM::DIM_2D;
		s->width = 8;
		s->height = 8;
		s->depth = 1;
		s->numLevels = 0;
		s->format = Latte::E_GX2SURFFMT::R8_G8_B8_A8_UNORM;
		s->aa = 0;
		s->imagePtr = MEMORY_TILINGAPERTURE_AREA_ADDR;
		s->tileMode = E_GX2TILEMODE::TM_2D_TILED_THIN1;
		s->swizzle = 0;
		s->alignment = 0x400;
		s->pitch = 8;
	}

	void GX2CalcSurfaceSizeAndAlignment(GX2Surface* surface)
	{
		_FixGarbageSurface(surface);
		const bool special32 = surface->tileMode == E_GX2TILEMODE::TM_32_SPECIAL;
		// "default" tile modes let GX2 pick: macro tiling for everything except plain 1D textures
		bool autoTileMode = false;
		if (surface->tileMode == E_GX2TILEMODE::TM_LINEAR_GENERAL || special32)
		{
			const bool needsTiling = surface->dim != Latte::E_DIM::DIM_1D || (surface->resFlag & GX2_RESFLAG_USAGE_DEPTH_BUFFER) != 0 || surface->aa != 0;
			if (!needsTiling)
				surface->tileMode = E_GX2TILEMODE::TM_LINEAR_ALIGNED;
			else
			{
				const bool thick = surface->dim == Latte::E_DIM::DIM_3D && (surface->resFlag & GX2_RESFLAG_USAGE_COLOR_BUFFER) == 0;
				surface->tileMode = thick ? E_GX2TILEMODE::TM_2D_TILED_THICK : E_GX2TILEMODE::TM_2D_TILED_THIN1;
				autoTileMode = true;
			}
		}
		E_GX2TILEMODE prevLevelTileMode = surface->tileMode;

		if (surface->numLevels == 0)
			surface->numLevels = 1;
		surface->numLevels = std::min<uint32>(surface->numLevels, _MaxLevels(surface));
		const uint32 levelCount = surface->numLevels;

		// macro tiled surfaces start out swizzled down to level 13 (i.e. all levels)
		_ClearSwizzleStopLevel(surface);
		if (Latte::TM_IsMacroTiled(surface->tileMode))
			surface->swizzle = surface->swizzle | (13u << 16);

		// for the special 32 mode the "too small for macro tiles" test uses 4x the alignment for compressed formats
		const uint32 smallShift = (special32 && Latte::IsCompressedFormat(surface->format)) ? 2 : 0;

		LatteAddrLib::AddrSurfaceInfo_OUT info = {};
		surface->mipOffset[0] = 0;
		uint32 level1Offset = 0;
		uint32 prevLevelSize = 0;
		for (uint32 level = 0; level < levelCount; level++)
		{
			GX2CalculateSurfaceInfo(surface, level, &info);
			if (level == 0)
			{
				if (autoTileMode)
				{
					// addrlib may degrade the tile mode (e.g. for tiny surfaces)
					if (surface->tileMode != (E_GX2TILEMODE)info.hwTileMode)
					{
						surface->tileMode = (E_GX2TILEMODE)info.hwTileMode;
						GX2CalculateSurfaceInfo(surface, 0, &info);
						if (!Latte::TM_IsMacroTiled(surface->tileMode))
							_ClearSwizzleStopLevel(surface);
						prevLevelTileMode = surface->tileMode;
					}
					// smaller than one macro tile in both directions: use micro tiling instead
					if (surface->width < (info.pitchAlign << smallShift) && surface->height < (info.heightAlign << smallShift))
					{
						surface->tileMode = surface->tileMode == E_GX2TILEMODE::TM_2D_TILED_THICK ? E_GX2TILEMODE::TM_1D_TILED_THICK : E_GX2TILEMODE::TM_1D_TILED_THIN1;
						GX2CalculateSurfaceInfo(surface, 0, &info);
						_ClearSwizzleStopLevel(surface);
						prevLevelTileMode = surface->tileMode;
					}
				}
				surface->imageSize = (uint32)info.surfSize;
				surface->alignment = info.baseAlign;
				surface->pitch = info.pitch;
			}
			else
			{
				uint32 padding = 0;
				if (Latte::TM_IsMacroTiled(prevLevelTileMode) && !Latte::TM_IsMacroTiled(info.hwTileMode))
				{
					// first level that is no longer macro tiled: it is not swizzled, remember where swizzling stops
					surface->swizzle = (surface->swizzle & 0xFF00FFFF) | (level << 16);
					prevLevelTileMode = (E_GX2TILEMODE)info.hwTileMode;
					if (level > 1)
						padding = surface->swizzle & 0xFFFF;
				}
				padding += (info.baseAlign - prevLevelSize % info.baseAlign) % info.baseAlign;
				// offsets are relative to mipPtr (level 1 is at offset 0 while the chain is built)
				if (level == 1)
					level1Offset = prevLevelSize + padding;
				else
					surface->mipOffset[level - 1] = prevLevelSize + padding + surface->mipOffset[level - 2];
			}
			prevLevelSize = (uint32)info.surfSize;
		}
		surface->mipSize = levelCount > 1 ? prevLevelSize + surface->mipOffset[levelCount - 2] : 0;
		surface->mipOffset[0] = level1Offset;

		if (surface->format == Latte::E_GX2SURFFMT::NV12_UNORM)
		{
			// NV12: the half-size chroma plane follows the luma plane (aligned) inside the image
			const uint32 align = surface->alignment;
			const uint32 lumaSize = surface->imageSize;
			surface->mipOffset[0] = lumaSize + (align - lumaSize % align) % align;
			surface->imageSize = surface->mipOffset[0] + (lumaSize >> 1);
		}
	}

	uint32 GX2GetSurfaceMipPitch(GX2Surface* surface, uint32 level)
	{
		LatteAddrLib::AddrSurfaceInfo_OUT info;
		GX2CalculateSurfaceInfo(surface, level, &info);
		return info.pitch;
	}

	uint32 GX2GetSurfaceMipSliceSize(GX2Surface* surface, uint32 level)
	{
		LatteAddrLib::AddrSurfaceInfo_OUT info;
		GX2CalculateSurfaceInfo(surface, level, &info);
		const uint32 samples = 1u << surface->aa;
		return samples * (info.bpp >> 3) * info.height * info.pitch;
	}

	// bits per texel (per 4x4 block divided by 16 for block compressed formats)
	uint32 GX2GetSurfaceFormatBits(Latte::E_GX2SURFFMT surfaceFormat)
	{
		const uint32 bits = Latte::GetFormatBits(surfaceFormat);
		return Latte::IsCompressedFormat(surfaceFormat) ? bits / 16 : bits;
	}

	uint32 GX2GetSurfaceSwizzleOffset(GX2Surface* surface, uint32 level)
	{
		const uint32 swizzle = surface->swizzle;
		const uint32 swizzleStopLevel = (swizzle >> 16) & 0xFF;
		if (Latte::TM_IsMacroTiled(surface->tileMode) && level < swizzleStopLevel)
			return swizzle & 0xFFFF;
		return 0;
	}

	uint32 GX2GetSurfaceSwizzle(GX2Surface* surface)
	{
		return (surface->swizzle >> 8) & 0xFF;
	}

	void GX2SetSurfaceSwizzle(GX2Surface* surface, uint32 newSwizzle)
	{
		// newSwizzle is deliberately not masked: some games pass values above 0xFF
		surface->swizzle = (surface->swizzle & ~0xFF00u) | (newSwizzle << 8);
	}

	uint32 GX2SurfaceIsCompressed(Latte::E_GX2SURFFMT surfaceFormat)
	{
		return Latte::IsCompressedFormat(surfaceFormat) ? GX2_TRUE : GX2_FALSE;
	}

	// HiZ and color aux (fast clear/compression) buffers are not emulated, report a small fixed size
	void GX2CalcDepthBufferHiZInfo(GX2DepthBuffer* depthBuffer, uint32be* sizeOut, uint32be* alignOut)
	{
		*sizeOut = 0x1000;
		*alignOut = 0x100;
	}

	void GX2CalcColorBufferAuxInfo(GX2ColorBuffer* colorBuffer, uint32be* sizeOut, uint32be* alignOut)
	{
		*sizeOut = 0x1000;
		*alignOut = 0x100;
	}

	uint32 GX2CheckSurfaceUseVsFormat(uint32 resFlags, uint32 surfaceFormat)
	{
		return 1; // every format/usage combination is accepted
	}

	Latte::E_ENDIAN_SWAP GetSurfaceFormatSwapMode(Latte::E_GX2SURFFMT fmt)
	{
		return Latte::E_ENDIAN_SWAP::SWAP_NONE; // GX2 surfaces are never endian swapped
	}

	// CB_COLORn_INFO.SOURCE_FORMAT: 1 = the shader export can be normalized, 0 = export at full precision
	uint32 GetSurfaceColorBufferExportFormat(Latte::E_GX2SURFFMT fmt)
	{
		// bit n set: hardware format n may use the normalized export
		constexpr uint64 kNormExportFormats = 0x003E10710FE11FDFull;
		return (uint32)((kNormExportFormats >> ((uint32)fmt & 0x3F)) & 1);
	}

	void GX2SurfaceInit()
	{
		cafeExportRegister("gx2", GX2GetSurfaceMipPitch, LogType::GX2);
		cafeExportRegister("gx2", GX2GetSurfaceFormatBits, LogType::GX2);
		cafeExportRegister("gx2", GX2GetSurfaceMipSliceSize, LogType::GX2);
		cafeExportRegister("gx2", GX2GetSurfaceSwizzleOffset, LogType::GX2);
		cafeExportRegister("gx2", GX2GetSurfaceSwizzle, LogType::GX2);
		cafeExportRegister("gx2", GX2SurfaceIsCompressed, LogType::GX2);
		cafeExportRegister("gx2", GX2CalcDepthBufferHiZInfo, LogType::GX2);
		cafeExportRegister("gx2", GX2CalcColorBufferAuxInfo, LogType::GX2);
		cafeExportRegister("gx2", GX2CalcSurfaceSizeAndAlignment, LogType::GX2);
		cafeExportRegister("gx2", GX2CheckSurfaceUseVsFormat, LogType::GX2);
		cafeExportRegister("gx2", GX2SetSurfaceSwizzle, LogType::GX2);
	}
};
