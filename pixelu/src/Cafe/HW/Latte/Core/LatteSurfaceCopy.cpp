// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteSurfaceCopy.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"

// GX2CopySurface. Guest memory and host VRAM are kept apart, and either one can hold
// the current contents of a surface. The copy is done on the GPU when the source is
// cached and in guest memory otherwise. The copy does not care about the pixel
// format (an R32F surface may be copied as RGBA8) and differing tile modes can be
// compatible, so the destination is looked up loosely.

void gx2SurfaceCopySoftware(
	uint8* inputData, sint32 surfSrcHeight, sint32 srcPitch, sint32 srcDepth, uint32 srcSlice, uint32 srcSwizzle, uint32 srcHwTileMode,
	uint8* outputData, sint32 surfDstHeight, sint32 dstPitch, sint32 dstDepth, uint32 dstSlice, uint32 dstSwizzle, uint32 dstHwTileMode,
	uint32 copyWidth, uint32 copyHeight, uint32 copyBpp);

// detiles/retiles directly in guest memory
static void LatteSurfaceCopy_CopyInRAM(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	const Latte::E_HWFMT dstFormat = Latte::GetHWFormat(dst.surfaceFormat);
	uint32 width = rect.width;
	uint32 height = rect.height;
	if (Latte::IsCompressedFormat(dstFormat))
	{
		width = (width + 3) / 4;
		height = (height + 3) / 4;
	}
	gx2SurfaceCopySoftware((uint8*)MEMPTR<void>(src.physDataAddr).GetPtr(), src.heightInTexels, src.pitch, 1, src.sliceIndex, src.swizzle, (uint32)src.tilemode,
		(uint8*)MEMPTR<void>(dst.physDataAddr).GetPtr(), dst.heightInTexels, dst.pitch, 1, dst.sliceIndex, dst.swizzle, (uint32)dst.tilemode,
		width, height, Latte::GetFormatBits(dstFormat));
}

static bool IsLinearTileMode(Latte::E_HWTILEMODE tm)
{
	return tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED || tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL;
}

// creates a texture for a destination surface that is not cached yet
static LatteTextureView* CreateDestinationView(const LatteSurfaceCopyParam& dst)
{
	uint32 width = dst.pitch;
	uint32 height = dst.heightInTexels;
	if (Latte::IsCompressedFormat(dst.surfaceFormat))
	{
		width *= 4;
		height *= 4;
	}
	// cubemaps need a multiple of six slices, so the target becomes a 2D (array) texture
	Latte::E_DIM dimBase = dst.dim;
	if (dimBase == Latte::E_DIM::DIM_CUBEMAP)
		dimBase = dst.sliceIndex > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D;
	const Latte::E_DIM dimView = Latte::IsMSAA(dst.dim) ? Latte::E_DIM::DIM_2D_MSAA : Latte::E_DIM::DIM_2D;
	return LatteTexture_CreateMapping(dst.physDataAddr, MPTR_NULL, width, height, dst.sliceIndex + 1, dst.pitch, Latte::MakeHWTileMode(dst.tilemode), dst.swizzle, 0, 1, dst.sliceIndex, 1, dst.surfaceFormat, dimBase, dimView, false);
}

void LatteSurfaceCopy_copySurfaceNew(const LatteSurfaceCopyParam& src, const LatteSurfaceCopyParam& dst, const LatteSurfaceCopyRect& rect)
{
	cemu_assert_debug(rect.x == 0 && rect.y == 0); // copies with an origin offset are not implemented
	cemu_assert_debug((rect.x + rect.width) <= dst.pitch * (Latte::IsCompressedFormat(dst.surfaceFormat) ? 4 : 1));
	cemu_assert_debug((rect.x + rect.width) <= src.pitch * (Latte::IsCompressedFormat(src.surfaceFormat) ? 4 : 1));
	const uint32 extentX = rect.x + rect.width;
	const uint32 extentY = rect.y + rect.height;

	// the special linear mode is never cached
	if (src.tilemode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL)
	{
		LatteSurfaceCopy_CopyInRAM(src, dst, rect);
		return;
	}
	// a source that is not cached is copied in guest memory
	LatteTextureView* sourceView = LatteTC_GetTextureSliceViewOrTryCreate(src.physDataAddr, MPTR_NULL, src.surfaceFormat, Latte::MakeHWTileMode(src.tilemode), extentX, extentY, 1, src.pitch, src.swizzle, src.sliceIndex, 0);
	if (!sourceView)
	{
		LatteSurfaceCopy_CopyInRAM(src, dst, rect);
		return;
	}
	LatteTexture* sourceTexture = sourceView->baseTexture;
	LatteTexture_UpdateDataToLatest(sourceTexture);
	if (sourceTexture->reloadFromDynamicTextures)
	{
		LatteTexture_UpdateCacheFromDynamicTextures(sourceTexture);
		sourceTexture->reloadFromDynamicTextures = false;
	}
	// copying a cached texture into the special linear mode reads it back right away
	if (dst.tilemode == Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL)
	{
		LatteTextureReadback_ReadbackToLinearBlocking(sourceView, MEMPTR<uint8>(dst.physDataAddr).GetPtr(), rect.width, rect.height, dst.pitch);
		return;
	}

	LatteTextureView* destinationView;
	{
		// creating the destination can absorb the source texture into the new one
		LatteTextureDeleteGuard sourceGuard(sourceTexture);
		destinationView = LatteTextureViewLookupCache::lookupSliceMinSize(dst.physDataAddr, extentX, extentY, dst.pitch, 0, dst.sliceIndex, dst.surfaceFormat);
		if (!destinationView)
			destinationView = CreateDestinationView(dst);
		if (sourceGuard.WasDeleted())
		{
			sourceView = LatteTextureViewLookupCache::lookupSliceMinSize(src.physDataAddr, extentX, extentY, src.pitch, 0, src.sliceIndex, src.surfaceFormat);
			if (!sourceView)
			{
				cemuLog_logDebug(LogType::Force, "LatteSurfaceCopy: source {:08x} was replaced while creating the destination, copy skipped", src.physDataAddr);
				return;
			}
			sourceTexture = sourceView->baseTexture;
		}
	}
	if (!destinationView)
		return;
	LatteTexture* destinationTexture = destinationView->baseTexture;

	LatteTC_MarkTextureStillInUse(destinationTexture);
	LatteTC_MarkTextureStillInUse(sourceTexture);
	if (LatteTexture_doesEffectiveRescaleRatioMatch(sourceTexture, sourceView->firstMip, destinationTexture, destinationView->firstMip))
	{
		if (sourceTexture->isDepth != destinationTexture->isDepth)
		{
			g_renderer->surfaceCopy_copySurfaceWithFormatConversion(sourceTexture, sourceView->firstMip, sourceView->firstSlice, destinationTexture, destinationView->firstMip, destinationView->firstSlice, rect.width, rect.height);
		}
		else
		{
			sint32 hostWidth = rect.width;
			sint32 hostHeight = rect.height;
			LatteTexture_scaleToEffectiveSize(sourceTexture, &hostWidth, &hostHeight, 0);
			g_renderer->texture_copyImageSubData(sourceTexture, sourceView->firstMip, 0, 0, src.sliceIndex, destinationTexture, destinationView->firstMip, 0, 0, destinationView->firstSlice, hostWidth, hostHeight, 1);
		}
		LatteTexture_MarkDynamicTextureAsChanged(destinationTexture->baseView, destinationView->firstSlice, destinationView->firstMip, LatteTexture_getNextUpdateEventCounter());
	}
	else
	{
		cemuLog_logDebug(LogType::Force, "LatteSurfaceCopy: source and destination have different resolution scales, copy skipped");
	}
	LatteTC_ResetTextureChangeTracker(destinationTexture);
	destinationTexture->lastUpdateEventCounter = LatteTexture_getNextUpdateEventCounter();
	LatteTC_FlagSliceAsGPUUpdated(destinationTexture, destinationView->firstSlice, destinationView->firstMip);

	// A tiled-to-linear copy usually prepares data for the CPU, so the result is read
	// back. Bayonetta 2 reads an 8x8 thin tiled surface the same way (Art Academy
	// does this too, probably without needing it).
	bool readback = !IsLinearTileMode(sourceTexture->tileMode) && IsLinearTileMode(destinationTexture->tileMode);
	if (destinationTexture->width == 8 && destinationTexture->height == 8 && destinationTexture->tileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THIN1)
		readback = true;
	if (readback)
		LatteTextureReadback_Initate(destinationView);
}
