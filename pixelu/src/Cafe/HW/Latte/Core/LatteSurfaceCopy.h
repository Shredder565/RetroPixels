// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// one side of a surface copy; address, size and pitch already refer to the copied mip level
struct LatteSurfaceCopyParam
{
	MPTR physDataAddr;
	uint32 swizzle;
	Latte::E_GX2SURFFMT surfaceFormat;
	sint32 heightInTexels;
	uint32 pitch;
	Latte::E_DIM dim;
	Latte::E_GX2TILEMODE tilemode;
	sint32 aa;
	sint32 sliceIndex;
};

struct LatteSurfaceCopyRect
{
	uint32 x;
	uint32 y;
	uint32 width; // texels
	uint32 height;
};

void LatteSurfaceCopy_copySurfaceNew(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect);
