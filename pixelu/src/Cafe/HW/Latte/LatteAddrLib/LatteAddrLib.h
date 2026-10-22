// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"

// Surface layout and tiling math for the Latte GPU (R7xx tiling with the
// Wii U memory configuration: 2 pipes, 4 banks, 256 byte pipe interleave).
namespace LatteAddrLib
{
	// memory configuration
	inline constexpr uint32 kNumPipes = 2;
	inline constexpr uint32 kNumBanks = 4;
	inline constexpr uint32 kPipeInterleaveBytes = 256; // bytes of one pipe before switching
	inline constexpr uint32 kRowSize = 2048;
	inline constexpr uint32 kSwapSize = 256;
	inline constexpr uint32 kSplitSize = 2048; // micro tiles above this size are split per sample group

	// unused by Latte, kept so AddrSurfaceInfo_OUT has a stable layout
	struct AddrTileInfo
	{
		uint32 banks;
		uint32 bankWidth;
		uint32 bankHeight;
		uint32 macroAspectRatio;
		uint32 tileSplitBytes;
		uint32 pipeConfig;
	};

	// result of a surface layout computation
	struct AddrSurfaceInfo_OUT
	{
		uint32 size; // sizeof(AddrSurfaceInfo_OUT)
		uint32 pitch; // in elements (blocks for BCn)
		uint32 height; // in elements
		uint32 depth; // slices
		uint64 surfSize; // bytes
		Latte::E_HWTILEMODE hwTileMode; // tile mode actually used for this level
		uint32 baseAlign;
		uint32 pitchAlign;
		uint32 heightAlign;
		uint32 depthAlign;
		uint32 bpp; // bits per element
		uint32 pixelPitch; // pitch in pixels
		uint32 pixelHeight; // height in pixels
		uint32 pixelBits;
		uint32 sliceSize;
		uint32 pitchTileMax;
		uint32 heightTileMax;
		uint32 sliceTileMax;
		AddrTileInfo* pTileInfo;
		uint32 tileType;
		sint32 tileIndex;
	};

	inline bool IsHWTileMode(Latte::E_GX2TILEMODE tileMode)
	{
		return static_cast<uint32>(tileMode) < 16;
	}

	inline bool IsValidHWTileMode(Latte::E_HWTILEMODE tileMode)
	{
		return static_cast<uint32>(tileMode) < 16;
	}

	// thick modes store 4 slices interleaved in each micro tile
	inline bool TM_IsThick(Latte::E_HWTILEMODE tileMode)
	{
		switch (tileMode)
		{
		case Latte::E_HWTILEMODE::TM_1D_TILED_THICK:
		case Latte::E_HWTILEMODE::TM_2D_TILED_THICK:
		case Latte::E_HWTILEMODE::TM_2B_TILED_THICK:
		case Latte::E_HWTILEMODE::TM_3D_TILED_THICK:
		case Latte::E_HWTILEMODE::TM_3B_TILED_THICK:
			return true;
		default:
			return false;
		}
	}

	inline uint32 TM_GetThickness(Latte::E_HWTILEMODE tileMode)
	{
		return TM_IsThick(tileMode) ? 4 : 1;
	}

	inline bool TM_IsThickAndMacroTiled(Latte::E_HWTILEMODE tileMode)
	{
		return TM_IsThick(tileMode) && tileMode != Latte::E_HWTILEMODE::TM_1D_TILED_THICK;
	}

	uint32 ComputeSurfaceRotationFromTileMode(Latte::E_HWTILEMODE tileMode);
	uint32 ComputeSurfaceBankSwappedWidth(Latte::E_HWTILEMODE tileMode, uint32 bpp, uint32 numSamples, uint32 pitch);

	// layout of one mip level as GX2CalcSurfaceSizeAndAlignment computes it
	void GX2CalculateSurfaceInfo(Latte::E_GX2SURFFMT surfaceFormat, uint32 surfaceWidth, uint32 surfaceHeight, uint32 surfaceDepth, Latte::E_DIM surfaceDim, Latte::E_GX2TILEMODE surfaceTileMode, uint32 surfaceAA, uint32 level, AddrSurfaceInfo_OUT* pSurfOut, bool optimizeForDepthBuffer = false, bool optimizeForScanBuffer = false);
	// offset of mip level mipIndex relative to the mip chain base (level 1)
	uint32 CalculateMipOffset(Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, Latte::E_DIM dim, Latte::E_HWTILEMODE tileMode, uint32 swizzle, uint32 surfaceAA, sint32 mipIndex);
	// address and size of one slice of one mip level; thick modes return the whole
	// 4-slice group and the slice within it in subSliceIndex
	void CalculateMipAndSliceAddr(uint32 physAddr, uint32 physMipAddr, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, Latte::E_DIM dim, Latte::E_HWTILEMODE tileMode, uint32 swizzle, uint32 surfaceAA, sint32 mipIndex, sint32 sliceIndex, uint32* outputSliceOffset, uint32* outputSliceSize, sint32* subSliceIndex);

	/* element addressing (byte offset of element x/y/slice/sample within a surface) */

	uint32 ComputeSurfaceAddrFromCoordLinear(uint32 x, uint32 y, uint32 slice, uint32 sample, uint32 bpp, uint32 pitch, uint32 height, uint32 numSlices);
	uint32 ComputeSurfaceAddrFromCoordMicroTiled(uint32 x, uint32 y, uint32 slice, uint32 bpp, uint32 pitch, uint32 height, Latte::E_HWTILEMODE tileMode, bool isDepth);
	uint32 ComputeSurfaceAddrFromCoordMacroTiled(uint32 x, uint32 y, uint32 slice, uint32 sample, uint32 bpp, uint32 pitch, uint32 height, uint32 numSamples, Latte::E_HWTILEMODE tileMode, bool isDepth, uint32 pipeSwizzle, uint32 bankSwizzle);

	// element index inside an 8x8(x4) micro tile; microTileType 1 = depth layout
	uint32 _ComputePixelIndexWithinMicroTile(uint32 x, uint32 y, uint32 z, uint32 bpp, Latte::E_HWTILEMODE tileMode, uint32 microTileType);

	// precomputed state for addressing many elements of the same macro tiled slice
	struct CachedSurfaceAddrInfo
	{
		// parameters
		uint32 slice;
		uint32 sample;
		uint32 bpp;
		uint32 pitch;
		uint32 height;
		uint32 depth;
		uint32 numSamples;
		Latte::E_HWTILEMODE tileMode;
		int isDepth;
		uint32 pipeSwizzle;
		uint32 bankSwizzle;
		// derived
		uint32 microTileType; // 1 for depth surfaces
		uint32 microTileThickness;
		uint32 microTileBits;
		uint32 microTileBytes;
		uint32 elementBits; // bits stepped per element index inside a micro tile
		uint32 macroTileWidth;
		uint32 macroTileHeight;
		uint32 macroTileWidthShift;
		uint32 macroTileHeightShift;
		uint32 macroTileBytes;
		uint32 bankSwapWidth;
		uint32 sliceBytes;
		uint32 bankPipeXor; // swizzle and slice rotation applied to pipe/bank
		uint16 microTilePixelIndexTable[8 * 8 * 8]; // [z][y][x]
	};

	void SetupCachedSurfaceAddrInfo(CachedSurfaceAddrInfo* info, uint32 slice, uint32 sample, uint32 bpp, uint32 pitch, uint32 height, uint32 depth, uint32 numSamples, Latte::E_HWTILEMODE tileMode, int isDepth, uint32 pipeSwizzle, uint32 bankSwizzle);
	uint32 ComputeSurfaceAddrFromCoordMacroTiledCached(uint32 x, uint32 y, CachedSurfaceAddrInfo* info);
	// fast path: tile mode 2D_TILED_THIN1 and a single sample
	uint32 ComputeSurfaceAddrFromCoordMacroTiledCached_tm04_sample1(uint32 x, uint32 y, CachedSurfaceAddrInfo* info);
};
