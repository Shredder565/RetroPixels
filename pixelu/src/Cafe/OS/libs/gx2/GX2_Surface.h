// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

// Guest layout of GX2Surface (wut: GX2Surface)
struct GX2Surface
{
	/* +0x000 */ betype<Latte::E_DIM> dim;
	/* +0x004 */ uint32be width;
	/* +0x008 */ uint32be height;
	/* +0x00C */ uint32be depth; // depth or array size
	/* +0x010 */ uint32be numLevels; // mip levels including the base level
	/* +0x014 */ betype<Latte::E_GX2SURFFMT> format;
	/* +0x018 */ uint32be aa; // log2 of the sample count
	/* +0x01C */ uint32be resFlag; // GX2_RESFLAG_* and GX2R_RESFLAG_*
	/* +0x020 */ uint32be imageSize;
	/* +0x024 */ uint32be imagePtr;
	/* +0x028 */ uint32be mipSize;
	/* +0x02C */ uint32be mipPtr;
	/* +0x030 */ betype<Latte::E_GX2TILEMODE> tileMode;
	/* +0x034 */ uint32be swizzle; // bits 8-10: bank/pipe swizzle, bits 16-23: first level that is no longer macro tiled
	/* +0x038 */ uint32be alignment;
	/* +0x03C */ uint32be pitch;
	/* +0x040 */ uint32be mipOffset[13]; // [0]: offset of level 1 from mipPtr, [n]: offset of level n+1
};

static_assert(sizeof(betype<Latte::E_DIM>) == 4);
static_assert(sizeof(betype<Latte::E_GX2TILEMODE>) == 4);
static_assert(offsetof(GX2Surface, imageSize) == 0x20);
static_assert(offsetof(GX2Surface, tileMode) == 0x30);
static_assert(offsetof(GX2Surface, mipOffset) == 0x40);
static_assert(sizeof(GX2Surface) == 0x74);

// Guest layout of GX2ColorBuffer
struct GX2ColorBuffer
{
	/* +0x00 */ GX2Surface surface;
	/* +0x74 */ uint32be viewMip;
	/* +0x78 */ uint32be viewFirstSlice;
	/* +0x7C */ uint32be viewNumSlices;
	/* +0x80 */ MEMPTR<void> auxData;
	/* +0x84 */ uint32be auxSize2;
	/* +0x88 */ uint32be reg_size; // CB_COLORn_SIZE
	/* +0x8C */ uint32be reg_info; // CB_COLORn_INFO
	/* +0x90 */ uint32be reg_view; // CB_COLORn_VIEW
	/* +0x94 */ uint32be reg_mask; // CB_COLORn_MASK
	/* +0x98 */ uint32be reg4;
};

static_assert(offsetof(GX2ColorBuffer, viewMip) == 0x74);
static_assert(offsetof(GX2ColorBuffer, reg_size) == 0x88);
static_assert(sizeof(GX2ColorBuffer) == 0x9C);

// Guest layout of GX2DepthBuffer
struct GX2DepthBuffer
{
	/* +0x00 */ GX2Surface surface;
	/* +0x74 */ uint32be viewMip;
	/* +0x78 */ uint32be viewFirstSlice;
	/* +0x7C */ uint32be viewNumSlices;
	/* +0x80 */ MEMPTR<void> hiZPtr;
	/* +0x84 */ uint32be hiZSize;
	/* +0x88 */ float32be clearDepth;
	/* +0x8C */ uint32be clearStencil;
	/* +0x90 */ uint32be reg_size; // DB_DEPTH_SIZE
	/* +0x94 */ uint32be reg_view; // DB_DEPTH_VIEW
	/* +0x98 */ uint32be reg_base;
	/* +0x9C */ uint32be reg_htile_surface;
	/* +0xA0 */ uint32be reg_prefetch_limit;
	/* +0xA4 */ uint32be reg_preload_control;
	/* +0xA8 */ uint32be reg_poly_offset_db_fmt_cntl;
};

static_assert(offsetof(GX2DepthBuffer, clearDepth) == 0x88);
static_assert(offsetof(GX2DepthBuffer, reg_size) == 0x90);
static_assert(sizeof(GX2DepthBuffer) == 0xAC);

namespace GX2
{
	// addrlib surface info of one mip level, honouring the depth/scan buffer usage flags
	void GX2CalculateSurfaceInfo(GX2Surface* surfacePtr, uint32 level, LatteAddrLib::AddrSurfaceInfo_OUT* pSurfOut);

	Latte::E_ENDIAN_SWAP GetSurfaceFormatSwapMode(Latte::E_GX2SURFFMT fmt);
	uint32 GetSurfaceColorBufferExportFormat(Latte::E_GX2SURFFMT fmt);

	void GX2CalcSurfaceSizeAndAlignment(GX2Surface* surface);

	void GX2SurfaceInit();
};
