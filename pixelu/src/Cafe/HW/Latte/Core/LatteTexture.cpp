// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

#include "Cafe/GraphicPack/GraphicPack2.h"

#include <boost/container/small_vector.hpp>

// Texture memory model
// --------------------
// Every subresource (one slice of one mip) of a cached texture covers a guest memory
// range. Those ranges are kept in a coarse spatial index so textures that alias the
// same memory can be found. Aliasing textures are tied together in two ways:
//  - relations: the subresources start at the same address with a compatible layout,
//    so GPU-side contents can be copied between them directly
//  - data overlaps: the ranges intersect in some other way; this is only used to tell
//    when the contents of a texture were completely overwritten by others

namespace
{
	// calls fn(sliceIndex, mipIndex, info) for every subresource, mip by mip
	template<typename TFunc>
	void ForEachSubresource(LatteTexture* texture, sint32 mipCount, TFunc&& fn)
	{
		for (sint32 mip = 0; mip < mipCount; mip++)
		{
			const sint32 sliceCount = texture->Is3DTexture() ? std::max(1, texture->depth >> mip) : texture->depth;
			for (sint32 slice = 0; slice < sliceCount; slice++)
				fn(slice, mip, texture->sliceMipInfo + texture->GetSliceMipArrayIndex(slice, mip));
		}
	}

	template<typename TFunc>
	void ForEachSubresource(LatteTexture* texture, TFunc&& fn)
	{
		ForEachSubresource(texture, texture->mipLevels, std::forward<TFunc>(fn));
	}

	// Spatial index over the 32-bit guest address space. The space is cut into equally
	// sized cells; a range is listed in every cell it touches, in the order ranges were
	// added.
	class OccupancyIndex
	{
	public:
		struct Entry
		{
			uint32 addrStart;
			uint32 addrEnd;
			LatteTextureSliceMipInfo* sliceMipInfo;
		};

		void Add(LatteTextureSliceMipInfo* info)
		{
			const Entry entry{ info->addrStart, info->addrEnd, info };
			ForEachCell(info->addrStart, info->addrEnd, [&](std::vector<Entry>& cell) { cell.push_back(entry); });
		}

		// drops all entries of the texture from the cells the range touches
		void RemoveTexture(LatteTexture* texture, uint32 addrStart, uint32 addrEnd)
		{
			ForEachCell(addrStart, addrEnd, [&](std::vector<Entry>& cell) {
				cell.erase(std::remove_if(cell.begin(), cell.end(), [texture](const Entry& e) { return e.sliceMipInfo->texture == texture; }), cell.end());
			});
		}

		// visits the entries of every cell touched by [addrStart, addrEnd). Ranges that
		// span several cells are visited once per cell.
		template<typename TFunc>
		void Visit(uint32 addrStart, uint32 addrEnd, TFunc&& fn)
		{
			ForEachCell(addrStart, addrEnd, [&](std::vector<Entry>& cell) {
				for (Entry& e : cell)
					fn(e);
			});
		}

	private:
		static constexpr uint32 kCellShift = 21; // 2MB per cell
		static constexpr uint32 kCellCount = 1u << (32 - kCellShift);

		template<typename TFunc>
		void ForEachCell(uint32 addrStart, uint32 addrEnd, TFunc&& fn)
		{
			const uint32 first = addrStart >> kCellShift;
			const uint32 last = (uint32)(addrEnd - 1) >> kCellShift;
			for (uint32 c = first; c <= last; c++)
				fn(m_cells[c]);
		}

		std::vector<Entry> m_cells[kCellCount];
	};

	OccupancyIndex& TextureMemory()
	{
		static OccupancyIndex s_index;
		return s_index;
	}
}

// --- texture cache snapshot for the debugging UI ---------------------------------
// The UI thread asks for a snapshot and the GPU thread produces it the next time it
// runs the texture garbage collector.

static std::atomic_bool s_cacheInfoRequested{ false };
static std::mutex s_cacheInfoMutex;
static std::vector<LatteTextureInformation> s_cacheInfo;

std::vector<LatteTextureInformation> LatteTexture_QueryCacheInfo()
{
	s_cacheInfoRequested.store(true);
	const auto start = std::chrono::steady_clock::now();
	while (s_cacheInfoRequested.load())
	{
		// give up after a second, the emulation may be paused
		if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(1000))
			return {};
		std::this_thread::yield();
	}
	std::lock_guard lock(s_cacheInfoMutex);
	return s_cacheInfo;
}

void LatteTexture_RefreshInfoCache()
{
	if (!s_cacheInfoRequested.load())
		return;
	std::vector<LatteTextureInformation> snapshot;
	for (LatteTexture* tex : LatteTexture::GetAllTextures())
	{
		// textures without registered views are not reachable through the cache
		if (!tex || tex->views.empty())
			continue;
		LatteTextureInformation& info = snapshot.emplace_back();
		info.physAddress = tex->physAddress;
		info.physMipAddress = tex->physMipAddress;
		info.width = tex->width;
		info.height = tex->height;
		info.depth = tex->depth;
		info.pitch = tex->pitch;
		info.mipLevels = tex->mipLevels;
		info.format = tex->format;
		info.isDepth = tex->isDepth;
		info.dim = tex->dim;
		info.tileMode = tex->tileMode;
		info.lastAccessTick = tex->lastAccessTick;
		info.lastAccessFrameCount = tex->lastAccessFrameCount;
		info.isUpdatedOnGPU = tex->isUpdatedOnGPU;
		info.alternativeViewCount = 0;
		info.overwriteInfo.hasResolutionOverwrite = tex->overwriteInfo.hasResolutionOverwrite;
		info.overwriteInfo.width = tex->overwriteInfo.width;
		info.overwriteInfo.height = tex->overwriteInfo.height;
		info.overwriteInfo.depth = tex->overwriteInfo.depth;
		for (LatteTextureView* view : tex->views)
		{
			if (view == tex->baseView)
				continue;
			LatteTextureViewInformation& v = info.views.emplace_back();
			v.physAddress = tex->physAddress;
			v.physMipAddress = tex->physMipAddress;
			v.width = tex->width;
			v.height = tex->height;
			v.pitch = tex->pitch;
			v.firstMip = view->firstMip;
			v.numMip = view->numMip;
			v.firstSlice = view->firstSlice;
			v.numSlice = view->numSlice;
			v.format = view->format;
			v.dim = view->dim;
		}
	}
	{
		std::lock_guard lock(s_cacheInfoMutex);
		s_cacheInfo = std::move(snapshot);
	}
	s_cacheInfoRequested.store(false);
}

// --- memory occupancy ------------------------------------------------------------

void LatteTexture_RegisterTextureMemoryOccupancy(LatteTexture* texture)
{
	ForEachSubresource(texture, [](sint32, sint32, LatteTextureSliceMipInfo* info) { TextureMemory().Add(info); });
}

void LatteTexture_UnregisterTextureMemoryOccupancy(LatteTexture* texture)
{
	ForEachSubresource(texture, [texture](sint32, sint32, LatteTextureSliceMipInfo* info) {
		TextureMemory().RemoveTexture(texture, info->addrStart, info->addrEnd);
	});
}

void LatteTexture_Invalidate(uint32 physAddr, uint32 size)
{
	// a whole-cache invalidation would rehash every texture; games use ranged invalidations for texture uploads
	if (size == 0xFFFFFFFF)
		return;
	std::vector<LatteTexture*> textures;
	LatteTC_LookupTexturesByPhysAddr(physAddr, textures);
	// pretend the last change check happened in an earlier frame so the next use checks again
	const uint32 staleFrame = LatteGPUState.frameCounter - 1;
	for (LatteTexture* tex : textures)
		tex->lastDataUpdateFrameCounter = staleFrame;
}

void LatteTexture_InitSliceAndMipInfo(LatteTexture* texture)
{
	cemu_assert_debug(texture->mipLevels > 0);
	cemu_assert_debug(texture->depth > 0);
	texture->sliceMipInfo = new LatteTextureSliceMipInfo[texture->GetSliceMipArraySize()]();
	ForEachSubresource(texture, [texture](sint32 slice, sint32 mip, LatteTextureSliceMipInfo* info) {
		uint32 addr, size;
		sint32 subIndex;
		LatteAddrLib::CalculateMipAndSliceAddr(texture->physAddress, texture->physMipAddress, texture->format, texture->width, texture->height, texture->depth, texture->dim, texture->tileMode, texture->swizzle, 0, mip, slice, &addr, &size, &subIndex);
		info->addrStart = addr;
		info->addrEnd = addr + size;
		info->subIndex = subIndex;
		info->dataChecksum = 0;
		info->sliceIndex = slice;
		info->mipIndex = mip;
		info->texture = texture;
		LatteAddrLib::AddrSurfaceInfo_OUT surfInfo;
		LatteAddrLib::GX2CalculateSurfaceInfo(texture->format, texture->width, texture->height, texture->depth, texture->dim, Latte::MakeGX2TileMode(texture->tileMode), 0, mip, &surfInfo);
		info->tileMode = surfInfo.hwTileMode;
		// the base level uses the pitch from the registers, smaller mips the computed one
		info->pitch = mip == 0 ? texture->pitch : (sint32)surfInfo.pitch;
		// the touched part of the range is an estimate that never exceeds the range
		uint32 estStart, estEnd;
		LatteTextureLoader_estimateAccessedDataRange(texture, slice, mip, estStart, estEnd);
		cemu_assert_debug(estStart >= info->addrStart && estEnd <= info->addrEnd && estStart <= estEnd);
		info->estDataAddrStart = estStart;
		info->estDataAddrEnd = estEnd;
	});
}

// --- format compatibility --------------------------------------------------------

// pairs (a, b) whose texture data must never be shared, checked in one direction
static bool IsBlockedViewFormatPair(Latte::E_GX2SURFFMT a, Latte::E_GX2SURFFMT b)
{
	using F = Latte::E_GX2SURFFMT;
	// copying 2_10_10_10 data to these formats misbehaves on OpenGL with Intel and AMD drivers
	if (Latte::GetHWFormat(a) == Latte::E_HWFMT::HWFMT_2_10_10_10 && (b == F::R11_G11_B10_FLOAT || b == F::R8_G8_B8_A8_UNORM))
		return true;
	// OpenGL has no view compatibility between A1B5G5R5 and other 16-bit formats (seen in N64 Virtual Console titles)
	if (a == F::A1_B5_G5_R5_UNORM)
		return true;
	// seen in Smash
	if ((a == F::D24_S8_UNORM || a == F::R32_FLOAT) && b == F::R10_G10_B10_A2_SNORM)
		return true;
	return false;
}

// false if textures of these formats must not exchange data even when their memory overlaps
static bool LatteTexture_IsFormatViewCompatible(Latte::E_GX2SURFFMT formatA, Latte::E_GX2SURFFMT formatB)
{
	// identical formats are always compatible, the default view of a texture depends on it
	if (formatA == formatB)
		return true;
	return !IsBlockedViewFormatPair(formatA, formatB) && !IsBlockedViewFormatPair(formatB, formatA);
}

static bool LatteTexture_IsTexelSizeCompatibleFormat(Latte::E_GX2SURFFMT formatA, Latte::E_GX2SURFFMT formatB)
{
	if (formatA == Latte::E_GX2SURFFMT::D24_S8_UNORM && formatB == Latte::E_GX2SURFFMT::D32_FLOAT)
		return false;
	// block compressed data of different encodings is never interchangeable
	if (Latte::IsCompressedFormat(formatA) && Latte::IsCompressedFormat(formatB) && Latte::GetHWFormat(formatA) != Latte::GetHWFormat(formatB))
		return false;
	return Latte::GetFormatBits(formatA) == Latte::GetFormatBits(formatB);
}

// --- copying between textures ----------------------------------------------------

// copies whole mips/slices into a recreated texture and carries over the update stamps
static void LatteTexture_copyData(LatteTexture* srcTexture, LatteTexture* dstTexture, sint32 mipCount, sint32 sliceCount)
{
	cemu_assert_debug(mipCount != 0 && sliceCount != 0);
	if (!LatteTexture_doesEffectiveRescaleRatioMatch(dstTexture, 0, srcTexture, 0))
	{
		cemuLog_logDebug(LogType::Force, "LatteTexture_copyData(): textures {:08x} and {:08x} have different resolution scales (texture rules), data is not copied", srcTexture->physAddress, dstTexture->physAddress);
		return;
	}
	sint32 copyWidth = srcTexture->width;
	sint32 copyHeight = srcTexture->height;
	LatteTexture_scaleToEffectiveSize(dstTexture, &copyWidth, &copyHeight, 0);
	for (sint32 mip = 0; mip < mipCount; mip++)
	{
		g_renderer->texture_copyImageSubData(srcTexture, mip, 0, 0, 0, dstTexture, mip, 0, 0, 0, std::max(copyWidth >> mip, 1), std::max(copyHeight >> mip, 1), sliceCount);
		const sint32 slicesInMip = dstTexture->Is3DTexture() ? (sliceCount >> mip) : sliceCount;
		for (sint32 slice = 0; slice < slicesInMip; slice++)
		{
			dstTexture->sliceMipInfo[dstTexture->GetSliceMipArrayIndex(slice, mip)].lastDynamicUpdate =
				srcTexture->sliceMipInfo[srcTexture->GetSliceMipArrayIndex(slice, mip)].lastDynamicUpdate;
		}
	}
}

// copies a rectangle of one subresource; coordinates are in guest texels and get scaled to the host size
static void LatteTexture_CopySlice(LatteTexture* srcTexture, sint32 srcSlice, sint32 srcMip, LatteTexture* dstTexture, sint32 dstSlice, sint32 dstMip, sint32 srcX, sint32 srcY, sint32 dstX, sint32 dstY, sint32 width, sint32 height)
{
	if (srcTexture->isDepth != dstTexture->isDepth)
	{
		g_renderer->surfaceCopy_copySurfaceWithFormatConversion(srcTexture, srcMip, srcSlice, dstTexture, dstMip, dstSlice, width, height);
		return;
	}
	sint32 hostWidth = width, hostHeight = height;
	LatteTexture_scaleToEffectiveSize(srcTexture, &hostWidth, &hostHeight, 0);
	sint32 hostSrcX = srcX, hostSrcY = srcY;
	LatteTexture_scaleToEffectiveSize(srcTexture, &hostSrcX, &hostSrcY, 0);
	sint32 hostDstX = dstX, hostDstY = dstY;
	LatteTexture_scaleToEffectiveSize(dstTexture, &hostDstX, &hostDstY, 0);
	if (!LatteTexture_doesEffectiveRescaleRatioMatch(dstTexture, 0, srcTexture, 0))
	{
		if (cemuLog_isLoggingEnabled(LogType::TextureCache))
		{
			sint32 srcEffW, srcEffH, dstEffW, dstEffH;
			srcTexture->GetEffectiveSize(srcEffW, srcEffH, 0);
			dstTexture->GetEffectiveSize(dstEffW, dstEffH, 0);
			cemuLog_log(LogType::Force, "Texture sync skipped, a texture rule gives the textures different scales:");
			cemuLog_log(LogType::Force, "  source {:08x} {}x{} -> {}x{} format {:04x} mip {}", srcTexture->physAddress, srcTexture->width, srcTexture->height, srcEffW, srcEffH, (uint32)srcTexture->format, srcMip);
			cemuLog_log(LogType::Force, "  target {:08x} {}x{} -> {}x{} format {:04x} mip {}", dstTexture->physAddress, dstTexture->width, dstTexture->height, dstEffW, dstEffH, (uint32)dstTexture->format, dstMip);
		}
		return;
	}
	g_renderer->texture_copyImageSubData(srcTexture, srcMip, hostSrcX, hostSrcY, srcSlice, dstTexture, dstMip, hostDstX, hostDstY, dstSlice, hostWidth, hostHeight, 1);
}

// finds the subresource of baseTexture that starts where subTexture's first subresource starts
static bool LatteTexture_GetSubtextureSliceAndMip(LatteTexture* baseTexture, LatteTexture* subTexture, sint32* baseSliceIndex, sint32* baseMipIndex)
{
	const LatteTextureSliceMipInfo* subFirst = subTexture->sliceMipInfo + subTexture->GetSliceMipArrayIndex(0, 0);
	for (sint32 mip = 0; mip < baseTexture->mipLevels; mip++)
	{
		const sint32 sliceCount = baseTexture->Is3DTexture() ? std::max(baseTexture->depth >> mip, 1) : baseTexture->depth;
		for (sint32 slice = 0; slice < sliceCount; slice++)
		{
			const LatteTextureSliceMipInfo* info = baseTexture->sliceMipInfo + baseTexture->GetSliceMipArrayIndex(slice, mip);
			if (info->addrStart == subFirst->addrStart && info->subIndex == subFirst->subIndex)
			{
				*baseSliceIndex = slice;
				*baseMipIndex = mip;
				return true;
			}
		}
	}
	return false;
}

void LatteTexture_MarkDynamicTextureAsChanged(LatteTextureView* textureView, sint32 sliceIndex, sint32 mipIndex, uint64 eventCounter)
{
	LatteTexture* tex = textureView->baseTexture;
	tex->lastWriteEventCounter = eventCounter;
	tex->GetSliceMipArrayEntry(textureView->firstSlice + sliceIndex, textureView->firstMip + mipIndex)->lastDynamicUpdate = eventCounter;
	// textures sharing the memory need to pick up the new contents
	LatteTexture_MarkConnectedTexturesForReloadFromDynamicTextures(tex);
}

// brings one subresource of dstTexture up to date with a subresource of srcTexture that shares its memory
static void LatteTexture_SyncSlice(LatteTexture* srcTexture, sint32 srcSliceIndex, sint32 srcMipIndex, LatteTexture* dstTexture, sint32 dstSliceIndex, sint32 dstMipIndex)
{
	// format overrides from texture rules have to agree, which keeps the copy paths simple
	if (srcTexture->overwriteInfo.hasFormatOverwrite != dstTexture->overwriteInfo.hasFormatOverwrite)
		return;
	if (srcTexture->overwriteInfo.hasFormatOverwrite && srcTexture->overwriteInfo.format != dstTexture->overwriteInfo.format)
		return;

	const bool srcIsCompressed = srcTexture->IsCompressedFormat();
	const bool dstIsCompressed = dstTexture->IsCompressedFormat();

	// A linear (or thin 1D tiled) texture that is an exact multiple of the target height
	// is treated as the target's slices stacked vertically. Ninja Gaiden fills a
	// 24x24x24 array through a 24x576 2D texture; CoD BO2 aliases a 1024x32 texture
	// with a 32x32x32 one.
	const bool stackCandidate = srcMipIndex == 0 && dstMipIndex == 0 &&
		(srcTexture->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED || srcTexture->tileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THIN1) &&
		srcTexture->height > dstTexture->height && (srcTexture->height % dstTexture->height) == 0;
	if (stackCandidate)
	{
		const bool layoutMatches = srcTexture->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED || srcTexture->width == 32;
		if (layoutMatches && !srcIsCompressed && !dstIsCompressed)
		{
			const sint32 stackedSlices = srcTexture->height / dstTexture->height;
			if (dstTexture->depth == stackedSlices)
			{
				const sint32 w = std::min(srcTexture->width, dstTexture->width);
				const sint32 h = std::min(srcTexture->height, dstTexture->height);
				for (sint32 i = 0; i < stackedSlices; i++)
					LatteTexture_CopySlice(srcTexture, srcSliceIndex, srcMipIndex, dstTexture, dstSliceIndex + i, dstMipIndex, 0, i * dstTexture->height, 0, 0, w, h);
			}
			return;
		}
	}

	// the target size is expressed in units of the source (blocks vs texels)
	sint32 dstWidth = dstTexture->width;
	sint32 dstHeight = dstTexture->height;
	if (srcIsCompressed && !dstIsCompressed)
	{
		dstWidth <<= 2;
		dstHeight <<= 2;
	}
	else if (!srcIsCompressed && dstIsCompressed)
	{
		dstWidth >>= 2;
		dstHeight >>= 2;
	}
	const sint32 srcW = std::max(srcTexture->width >> srcMipIndex, 1);
	const sint32 srcH = std::max(srcTexture->height >> srcMipIndex, 1);
	const sint32 dstW = std::max(dstWidth >> dstMipIndex, 1);
	const sint32 dstH = std::max(dstHeight >> dstMipIndex, 1);
	LatteTexture_CopySlice(srcTexture, srcSliceIndex, srcMipIndex, dstTexture, dstSliceIndex, dstMipIndex, 0, 0, 0, 0, std::min(srcW, dstW), std::min(srcH, dstH));
}

void LatteTexture_UpdateTextureFromDynamicChanges(LatteTexture* texture)
{
	// for every related texture, copy each shared subresource that was written more recently over there
	for (LatteTextureRelation* rel : texture->list_compatibleRelations)
	{
		LatteTexture* base = rel->baseTexture;
		LatteTexture* sub = rel->subTexture;
		const bool targetIsBase = texture == base;
		for (sint32 mip = 0; mip < rel->mipCount; mip++)
		{
			sint32 slices = rel->sliceCount;
			if (base->Is3DTexture())
			{
				cemu_assert_debug(mip == 0);
				slices >>= mip;
			}
			for (sint32 slice = 0; slice < slices; slice++)
			{
				const sint32 baseSlice = rel->baseSliceIndex + slice;
				const sint32 baseMip = rel->baseMipIndex + mip;
				LatteTextureSliceMipInfo* baseInfo = base->sliceMipInfo + base->GetSliceMipArrayIndex(baseSlice, baseMip);
				LatteTextureSliceMipInfo* subInfo = sub->sliceMipInfo + sub->GetSliceMipArrayIndex(slice, mip);
				if (targetIsBase)
				{
					if (baseInfo->lastDynamicUpdate >= subInfo->lastDynamicUpdate)
						continue;
					LatteTexture_SyncSlice(sub, slice, mip, base, baseSlice, baseMip);
					baseInfo->lastDynamicUpdate = subInfo->lastDynamicUpdate;
					if (sub->isUpdatedOnGPU)
						LatteTC_FlagSliceAsGPUUpdated(texture, baseInfo->sliceIndex, baseInfo->mipIndex);
				}
				else
				{
					if (subInfo->lastDynamicUpdate >= baseInfo->lastDynamicUpdate)
						continue;
					LatteTexture_SyncSlice(base, baseSlice, baseMip, sub, slice, mip);
					subInfo->lastDynamicUpdate = baseInfo->lastDynamicUpdate;
					if (base->isUpdatedOnGPU)
						LatteTC_FlagSliceAsGPUUpdated(texture, subInfo->sliceIndex, subInfo->mipIndex);
				}
			}
		}
	}
}

// --- relations -------------------------------------------------------------------

static bool LatteTexture_IsTileModeCompatible(LatteTexture* textureA, sint32 mipA, LatteTexture* textureB, sint32 mipB)
{
	if (mipA == 0 && mipB == 0)
		return textureA->tileMode == textureB->tileMode;
	// smaller mips can switch to another tile mode, compare the per-level modes
	return textureA->GetSliceMipArrayEntry(0, mipA)->tileMode == textureB->GetSliceMipArrayEntry(0, mipB)->tileMode;
}

// one direction of the format combinations whose relation is refused
static bool IsRefusedRelationFormatPair(LatteTexture* a, LatteTexture* b)
{
	// depth->color copies only fill the red channel, so D32 cannot stand in for RGBA8 data (Smash)
	if (a->isDepth && !b->isDepth && a->format == Latte::E_GX2SURFFMT::D32_FLOAT && Latte::GetHWFormat(b->format) == Latte::E_HWFMT::HWFMT_8_8_8_8)
		return true;
#ifdef ENABLE_VULKAN
	// Vulkan cannot copy between D32 and 8_24 images (Smash, Wii Fit stage)
	if (g_renderer->GetType() == RendererAPI::Vulkan && a->format == Latte::E_GX2SURFFMT::D32_FLOAT && Latte::GetHWFormat(b->format) == Latte::E_HWFMT::HWFMT_8_24)
		return true;
#endif
	return false;
}

// registers that texture2 lies inside texture1's memory with a layout that allows direct copies
static void LatteTexture_TrackTextureRelation(LatteTexture* texture1, LatteTexture* texture2)
{
	// the texture with the lower address is the base
	if (texture1->physAddress > texture2->physAddress)
		std::swap(texture1, texture2);
	cemu_assert_debug(texture1->physAddress != 0 && texture2->physAddress != 0);
	for (LatteTextureRelation* rel : texture1->list_compatibleRelations)
	{
		if (rel->baseTexture == texture1 && rel->subTexture == texture2)
			return;
	}
	if (IsRefusedRelationFormatPair(texture1, texture2) || IsRefusedRelationFormatPair(texture2, texture1))
		return;
	sint32 baseSlice = 0, baseMip = 0;
	if (texture1->physAddress != texture2->physAddress && !LatteTexture_GetSubtextureSliceAndMip(texture1, texture2, &baseSlice, &baseMip))
		return;
	// only a single shared mip level is tracked
	if (!LatteTexture_IsTileModeCompatible(texture1, baseMip, texture2, 0))
		return;
	if (texture1->GetSliceMipArrayEntry(baseSlice, baseMip)->pitch != texture2->GetSliceMipArrayEntry(0, 0)->pitch)
		return;
	const sint32 remainingDepth = texture1->GetMipDepth(baseMip) - baseSlice;
	cemu_assert_debug(remainingDepth >= 0);
	const sint32 sharedSlices = std::min(remainingDepth, texture2->depth);
	cemu_assert_debug(sharedSlices > 0);

	LatteTextureRelation* rel = new LatteTextureRelation{};
	rel->baseTexture = texture1;
	rel->subTexture = texture2;
	rel->baseSliceIndex = baseSlice;
	rel->baseMipIndex = baseMip;
	rel->sliceCount = sharedSlices;
	rel->mipCount = 1;
	rel->yOffset = 0;
	texture1->list_compatibleRelations.push_back(rel);
	texture2->list_compatibleRelations.push_back(rel);
}

void LatteTexture_DeleteTextureRelations(LatteTexture* texture)
{
	while (!texture->list_compatibleRelations.empty())
	{
		LatteTextureRelation* rel = texture->list_compatibleRelations.front();
		vectorRemoveByValue(rel->baseTexture->list_compatibleRelations, rel);
		vectorRemoveByValue(rel->subTexture->list_compatibleRelations, rel);
		delete rel;
	}
}

// --- data overlaps ---------------------------------------------------------------

static void LatteTexture_TrackDataOverlap(LatteTextureSliceMipInfo* info, LatteTextureSliceMipInfo* other)
{
	if (!(info->addrEnd > other->addrStart && info->addrStart < other->addrEnd))
		return;
	for (const auto& overlap : info->list_dataOverlap)
	{
		if (overlap.destMipSliceInfo == other)
			return;
	}
	info->list_dataOverlap.push_back({ other->texture, other });
	other->list_dataOverlap.push_back({ info->texture, info });
}

void LatteTexture_DeleteDataOverlapTracking(LatteTexture* texture)
{
	ForEachSubresource(texture, [texture](sint32, sint32, LatteTextureSliceMipInfo* info) {
		// remove the back references held by the other side of each overlap
		for (const auto& overlap : info->list_dataOverlap)
		{
			auto& backRefs = overlap.destMipSliceInfo->list_dataOverlap;
			cemu_assert_debug(std::none_of(backRefs.begin(), backRefs.end(), [&](const LatteTextureSliceMipDataOverlap_t& e) { return e.destTexture == overlap.destTexture; }));
			backRefs.erase(std::remove_if(backRefs.begin(), backRefs.end(), [texture](const LatteTextureSliceMipDataOverlap_t& e) { return e.destTexture == texture; }), backRefs.end());
		}
		info->list_dataOverlap.clear();
	});
}

// finds every cached texture sharing memory with the texture and records relations and overlaps
static void LatteTexture_GatherTextureRelations(LatteTexture* texture)
{
	ForEachSubresource(texture, [texture](sint32, sint32, LatteTextureSliceMipInfo* info) {
		TextureMemory().Visit(info->addrStart, info->addrEnd, [&](OccupancyIndex::Entry& entry) {
			LatteTextureSliceMipInfo* other = entry.sliceMipInfo;
			LatteTexture* otherTex = other->texture;
			if (otherTex == texture)
				return;
			if (!(info->addrEnd >= entry.addrStart && info->addrStart < entry.addrEnd))
				return;
			if (info->addrStart != entry.addrStart || info->subIndex != other->subIndex)
			{
				LatteTexture_TrackDataOverlap(info, other);
				return;
			}
			// both subresources start at the same address
			const bool layoutMatches = info->pitch == other->pitch && info->tileMode == other->tileMode;
			if (layoutMatches && LatteTexture_IsTexelSizeCompatibleFormat(texture->format, otherTex->format) && LatteTexture_IsFormatViewCompatible(texture->format, otherTex->format))
				LatteTexture_TrackTextureRelation(texture, otherTex);
		});
	});
}

// --- views into existing textures ------------------------------------------------

enum class ViewFit
{
	Fits, // the request can be served by a view of the texture
	NeedsLargerBase, // it would fit if the texture had more slices or mips
	Incompatible,
};

// whether a view of dimension viewDim may be created for a texture of dimension baseDim
static bool IsDimensionCompatibleForGX2View(Latte::E_DIM baseDim, Latte::E_DIM viewDim)
{
	using D = Latte::E_DIM;
	if (baseDim == viewDim)
		return true;
	switch (baseDim)
	{
	case D::DIM_2D:
		return viewDim == D::DIM_2D_ARRAY;
	case D::DIM_2D_ARRAY:
		return viewDim == D::DIM_2D || viewDim == D::DIM_CUBEMAP; // a 3D view of an array is not possible on OpenGL
	case D::DIM_CUBEMAP:
		return viewDim == D::DIM_2D || viewDim == D::DIM_2D_ARRAY;
	case D::DIM_3D:
		return viewDim == D::DIM_2D_ARRAY;
	default:
		// MSAA <-> 2D, 1D <-> 2D, 2D -> 3D and 2D -> cubemap occur in games but cannot be views
		return false;
	}
}

// common checks once the subresource at the requested address is known
static ViewFit CheckViewFormatAndDim(LatteTexture* tex, Latte::E_DIM viewDim, Latte::E_GX2SURFFMT format, bool allowAs3DView)
{
	if (!allowAs3DView && !IsDimensionCompatibleForGX2View(tex->dim, viewDim))
		return ViewFit::Incompatible;
	if (tex->isDepth && tex->format != format)
	{
		cemuLog_logDebug(LogType::Force, "Texture view: depth textures cannot be viewed with another format");
		return ViewFit::Incompatible;
	}
	// AMD's OpenGL driver ignores the format of a view bound as render target, so views always keep the texture format
	if (tex->format != format)
		return ViewFit::Incompatible;
	return ViewFit::Fits;
}

static ViewFit LatteTexture_CanTextureBeRepresentedAsView(LatteTexture* tex, uint32 physAddr, sint32 width, sint32 height, sint32 pitch, Latte::E_DIM viewDim, Latte::E_GX2SURFFMT format, bool isDepth, sint32 firstMip, sint32 numMip, sint32 firstSlice, sint32 numSlice, sint32& relativeMipIndex, sint32& relativeSliceIndex)
{
	relativeMipIndex = 0;
	relativeSliceIndex = 0;
	// a texture with a rule-overridden format only aliases views of its own format
	if (tex->overwriteInfo.hasFormatOverwrite && tex->format != format)
		return ViewFit::Incompatible;
	if (!LatteTexture_IsFormatViewCompatible(tex->format, format))
		return ViewFit::Incompatible;

	if (tex->physAddress == physAddr && tex->pitch == pitch)
	{
		// the request starts at the texture's first subresource
		if (tex->isDepth != isDepth)
			return ViewFit::Incompatible; // OpenGL cannot mix depth and color views
		if (!LatteTexture_IsTexelSizeCompatibleFormat(tex->format, format) || tex->width != width || tex->height != height)
			return ViewFit::Incompatible;
		// a 3D view of a 3D texture only works (on Vulkan) when it covers the whole texture
		const bool whole3DView = viewDim == Latte::E_DIM::DIM_3D && tex->dim == viewDim && firstSlice == 0 && firstMip == 0 && tex->mipLevels == numMip && tex->depth == numSlice;
		const ViewFit fit = CheckViewFormatAndDim(tex, viewDim, format, whole3DView);
		if (fit != ViewFit::Fits)
			return fit;
		if ((firstMip + numMip) > tex->mipLevels || (firstSlice + numSlice) > tex->depth)
			return ViewFit::NeedsLargerBase;
		return ViewFit::Fits;
	}

	// the request may start at a later mip or slice of the texture
	if (numMip > 1)
		return ViewFit::Incompatible;
	if (tex->Is3DTexture())
		return ViewFit::Incompatible; // views into 3D textures are not supported
	if (LatteAddrLib::TM_IsThickAndMacroTiled(tex->tileMode))
		return ViewFit::Incompatible; // thick tiles interleave slices, not handled
	for (sint32 mip = 0; mip < tex->mipLevels; mip++)
	{
		if (tex->GetSliceMipArrayEntry(0, mip)->pitch != pitch)
			continue;
		for (sint32 slice = 0; slice < tex->GetMipDepth(mip); slice++)
		{
			const LatteTextureSliceMipInfo* info = tex->GetSliceMipArrayEntry(slice, mip);
			if (info->addrStart != physAddr || info->pitch != pitch)
				continue;
			// first subresource at the requested address decides
			if (tex->isDepth != isDepth)
				return ViewFit::Incompatible;
			if (tex->GetMipWidth(mip) != width || tex->GetMipHeight(mip) != height)
				return ViewFit::Incompatible;
			if (!LatteTexture_IsTexelSizeCompatibleFormat(tex->format, format))
				return ViewFit::Incompatible;
			const ViewFit fit = CheckViewFormatAndDim(tex, viewDim, format, false);
			if (fit != ViewFit::Fits)
				return fit;
			relativeMipIndex = mip;
			relativeSliceIndex = slice;
			if ((mip + firstMip + numMip) > tex->mipLevels || (slice + firstSlice + numSlice) > tex->depth)
				return ViewFit::NeedsLargerBase;
			return ViewFit::Fits;
		}
	}
	return ViewFit::Incompatible;
}

// deletes related textures that are now fully covered by a view of the texture
static void LatteTexture_DeleteAbsorbedSubtextures(LatteTexture* texture)
{
	// Deleting a texture removes its relation from texture's list while the index still
	// advances, so the relation that moves into the freed slot is not checked this time.
	for (size_t i = 0; i < texture->list_compatibleRelations.size(); i++)
	{
		LatteTextureRelation* rel = texture->list_compatibleRelations[i];
		LatteTexture* other = rel->baseTexture != texture ? rel->baseTexture : rel->subTexture;
		sint32 relMip, relSlice;
		if (LatteTexture_CanTextureBeRepresentedAsView(texture, other->physAddress, other->width, other->height, other->pitch, other->dim, other->format, other->isDepth, 0, other->mipLevels, 0, other->depth, relMip, relSlice) == ViewFit::Fits)
		{
			LatteTexture_Delete(other);
			LatteGPUState.repeatTextureInitialization = true;
		}
	}
}

// links a freshly created texture into the cache and lets it inherit GPU-side contents
static void LatteTexture_IntegrateNewTexture(LatteTexture* texture)
{
	LatteTexture_GatherTextureRelations(texture);
	LatteTexture_UpdateTextureFromDynamicChanges(texture);
	LatteTexture_DeleteAbsorbedSubtextures(texture);
}

// replaces a texture with a copy that has more slices and/or mip levels
static void LatteTexture_RecreateTextureWithDifferentMipSliceCount(LatteTexture* texture, MPTR physMipAddr, sint32 newMipCount, sint32 newDepth)
{
	Latte::E_DIM newDim = texture->dim;
	if (newDepth > 1 && newDim == Latte::E_DIM::DIM_2D)
		newDim = Latte::E_DIM::DIM_2D_ARRAY;
	else if (newDepth > 1 && newDim == Latte::E_DIM::DIM_1D)
		newDim = Latte::E_DIM::DIM_1D_ARRAY;
	LatteTextureView* view = LatteTexture_CreateTexture(newDim, texture->physAddress, physMipAddr, texture->format, texture->width, texture->height, newDepth, texture->pitch, newMipCount, texture->swizzle, texture->tileMode, texture->isDepth);
	// more mips need a mip chain address
	cemu_assert(!(view->baseTexture->mipLevels <= 1 && physMipAddr == MPTR_NULL && newMipCount > 1));
	if (texture->isUpdatedOnGPU)
	{
		// the old texture holds data that guest memory does not
		LatteTexture_copyData(texture, view->baseTexture, texture->mipLevels, texture->depth);
		LatteTC_FlagSliceAsGPUUpdated(view->baseTexture, view->firstSlice, view->firstMip);
	}
	LatteTexture_Delete(texture);
	LatteTexture_IntegrateNewTexture(view->baseTexture);
}

LatteTextureView* LatteTexture_CreateMapping(MPTR physAddr, MPTR physMipAddr, sint32 width, sint32 height, sint32 depth, sint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle, sint32 firstMip, sint32 numMip, sint32 firstSlice, sint32 numSlice, Latte::E_GX2SURFFMT format, Latte::E_DIM dimBase, Latte::E_DIM dimView, bool isDepth, bool allowCreateNewDataTexture)
{
	if (format == Latte::E_GX2SURFFMT::INVALID_FORMAT)
	{
		cemuLog_logDebug(LogType::Force, "LatteTexture_CreateMapping(): invalid format");
		return nullptr;
	}
	cemu_assert_debug(depth);
	cemu_assert_debug(!(depth > 1 && dimBase == Latte::E_DIM::DIM_2D));
	cemu_assert_debug(!(numSlice > 1 && dimView == Latte::E_DIM::DIM_2D));

	// collect cached textures with a subresource starting where one of the requested slices starts
	boost::container::small_vector<LatteTexture*, 16> candidates;
	const sint32 sliceCount = firstSlice + numSlice;
	for (sint32 slice = 0; slice < sliceCount; slice++)
	{
		uint32 sliceAddr, sliceSize;
		sint32 subIndex;
		LatteAddrLib::CalculateMipAndSliceAddr(physAddr, physMipAddr, format, width, height, depth, dimBase, tileMode, swizzle, 0, 0, slice, &sliceAddr, &sliceSize, &subIndex);
		const uint32 sliceEnd = sliceAddr + sliceSize;
		TextureMemory().Visit(sliceAddr, sliceEnd, [&](OccupancyIndex::Entry& entry) {
			// ranges that overlap with an offset are not considered
			if (sliceEnd >= entry.addrStart && sliceAddr < entry.addrEnd && sliceAddr == entry.addrStart)
			{
				LatteTexture* tex = entry.sliceMipInfo->texture;
				if (std::find(candidates.begin(), candidates.end(), tex) == candidates.end())
					candidates.push_back(tex);
			}
		});
	}

	for (LatteTexture* tex : candidates)
	{
		sint32 relMip, relSlice;
		const ViewFit fit = LatteTexture_CanTextureBeRepresentedAsView(tex, physAddr, width, height, pitch, dimView, format, isDepth, firstMip, numMip, firstSlice, numSlice, relMip, relSlice);
		if (fit == ViewFit::Fits)
		{
			LatteTextureView* view = tex->GetOrCreateView(dimView, format, relMip + firstMip, numMip, relSlice + firstSlice, numSlice);
			// a view reached through a later subresource needs a lookup entry for that address
			if (relMip != 0 || relSlice != 0)
				view->CreateLookupForSubTexture(relMip, relSlice);
			return view;
		}
		// an aliasing texture that cannot serve the request means a separate texture is needed
		allowCreateNewDataTexture = true;
		if (fit == ViewFit::Incompatible)
			continue;
		// NeedsLargerBase: only growing a texture at its first subresource is supported
		if (relMip != 0 || relSlice != 0)
			continue;
		const sint32 newDepth = std::max(relSlice + firstSlice + numSlice, std::max(depth, tex->depth));
		const sint32 newMipCount = std::max(relMip + firstMip + numMip, tex->mipLevels);
		const MPTR newMipAddr = (relMip + firstMip + numMip) > 1 ? physMipAddr : tex->physMipAddress;
		LatteTexture_RecreateTextureWithDifferentMipSliceCount(tex, newMipAddr, newMipCount, newDepth);
		// the grown texture can now provide the view
		return LatteTexture_CreateMapping(physAddr, physMipAddr, width, height, depth, pitch, tileMode, swizzle, firstMip, numMip, firstSlice, numSlice, format, dimBase, dimView, isDepth);
	}

	if (!allowCreateNewDataTexture)
		return nullptr;
	LatteTextureView* baseView = LatteTexture_CreateTexture(dimBase, physAddr, physMipAddr, format, width, height, depth, pitch, firstMip + numMip, swizzle, tileMode, isDepth);
	LatteTexture* newTexture = baseView->baseTexture;
	LatteTexture_IntegrateNewTexture(newTexture);
	sint32 relMip, relSlice;
	const ViewFit fit = LatteTexture_CanTextureBeRepresentedAsView(newTexture, physAddr, width, height, pitch, dimView, format, isDepth, firstMip, numMip, firstSlice, numSlice, relMip, relSlice);
	cemu_assert(fit == ViewFit::Fits);
	return newTexture->GetOrCreateView(dimView, format, relMip + firstMip, numMip, relSlice + firstSlice, numSlice);
}

// --- lookups by guest address ----------------------------------------------------

// calls fn(texture) for each subresource entry that starts exactly at physAddr
template<typename TFunc>
static void VisitTexturesStartingAt(MPTR physAddr, TFunc&& fn)
{
	TextureMemory().Visit(physAddr, physAddr + 1, [&](OccupancyIndex::Entry& entry) {
		if (entry.addrStart == physAddr)
			fn(entry.sliceMipInfo->texture);
	});
}

// Iterates the textures whose base address and pitch match. *searchIndex selects the
// match to return and is advanced, so repeated calls walk through all matches.
LatteTextureView* LatteTC_LookupTextureByData(MPTR physAddr, sint32 width, sint32 height, sint32 pitch, sint32 firstMip, sint32 numMip, sint32 firstSlice, sint32 numSlice, sint32* searchIndex)
{
	cemu_assert_debug(firstMip == 0);
	sint32 matchIndex = 0;
	LatteTextureView* result = nullptr;
	VisitTexturesStartingAt(physAddr, [&](LatteTexture* tex) {
		if (result || tex->physAddress != physAddr || tex->pitch != pitch)
			return;
		if (firstSlice < 0 || firstSlice >= tex->depth)
			return;
		if (matchIndex >= *searchIndex)
		{
			(*searchIndex)++;
			result = tex->baseView;
			return;
		}
		matchIndex++;
	});
	return result;
}

void LatteTC_LookupTexturesByPhysAddr(MPTR physAddr, std::vector<LatteTexture*>& list_textures)
{
	VisitTexturesStartingAt(physAddr, [&](LatteTexture* tex) {
		if (tex->physAddress == physAddr)
			vectorAppendUnique(list_textures, tex);
	});
}

// returns a view of an existing texture; a new texture is only created where an aliasing one forces it
LatteTextureView* LatteTC_GetTextureSliceViewOrTryCreate(MPTR srcImagePtr, MPTR srcMipPtr, Latte::E_GX2SURFFMT srcFormat, Latte::E_HWTILEMODE srcTileMode, uint32 srcWidth, uint32 srcHeight, uint32 srcDepth, uint32 srcPitch, uint32 srcSwizzle, uint32 srcSlice, uint32 srcMip, const bool requireExactResolution)
{
	LatteTextureView* view = requireExactResolution ?
		LatteTextureViewLookupCache::lookupSlice(srcImagePtr, srcWidth, srcHeight, srcPitch, srcMip, srcSlice, srcFormat) :
		LatteTextureViewLookupCache::lookupSliceMinSize(srcImagePtr, srcWidth, srcHeight, srcPitch, srcMip, srcSlice, srcFormat);
	if (view)
		return view;
	const Latte::E_DIM dimBase = srcDepth > 1 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D;
	return LatteTexture_CreateMapping(srcImagePtr, srcMipPtr, srcWidth, srcHeight, srcDepth, srcPitch, srcTileMode, srcSwizzle, srcMip, 1, srcSlice, 1, srcFormat, dimBase, Latte::E_DIM::DIM_2D, false, false);
}

void LatteTexture_UpdateDataToLatest(LatteTexture* texture)
{
	if (LatteTC_HasTextureChanged(texture))
		LatteTexture_ReloadData(texture);
	if (texture->reloadFromDynamicTextures)
	{
		LatteTexture_UpdateCacheFromDynamicTextures(texture);
		texture->reloadFromDynamicTextures = false;
	}
}

// --- LatteTexture ----------------------------------------------------------------

LatteTextureSliceMipInfo* LatteTexture::GetSliceMipArrayEntry(sint32 sliceIndex, sint32 mipIndex)
{
	return sliceMipInfo + GetSliceMipArrayIndex(sliceIndex, mipIndex);
}

namespace
{
	// slots of deleted textures are reused, newest hole first
	struct GlobalTextureList
	{
		std::vector<LatteTexture*> textures;
		std::vector<size_t> freeSlots;

		void Insert(LatteTexture* tex)
		{
			if (freeSlots.empty())
			{
				tex->globalListIndex = textures.size();
				textures.push_back(tex);
				return;
			}
			tex->globalListIndex = freeSlots.back();
			freeSlots.pop_back();
			textures[tex->globalListIndex] = tex;
		}

		void Remove(LatteTexture* tex)
		{
			cemu_assert_debug(tex->globalListIndex < textures.size() && textures[tex->globalListIndex] == tex);
			// the last slot is dropped instead of becoming a hole
			if (tex->globalListIndex + 1 == textures.size())
			{
				textures.pop_back();
				return;
			}
			textures[tex->globalListIndex] = nullptr;
			freeSlots.push_back(tex->globalListIndex);
		}
	};

	GlobalTextureList& AllTextures()
	{
		static GlobalTextureList s_list;
		return s_list;
	}

	bool FormatHasStencil(Latte::E_GX2SURFFMT format)
	{
		return format == Latte::E_GX2SURFFMT::D24_S8_UNORM || format == Latte::E_GX2SURFFMT::D24_S8_FLOAT || format == Latte::E_GX2SURFFMT::D32_S8_FLOAT;
	}

	template<typename T>
	bool ListExcludes(const std::vector<T>& list, T value)
	{
		return std::find(list.begin(), list.end(), value) == list.end();
	}

	// whether a [TextureRedefine] rule applies to a texture with these properties
	bool TextureRuleMatches(const GraphicPack2::TextureRule::FILTER_SETTINGS& filter, Latte::E_GX2SURFFMT format, Latte::E_HWTILEMODE tileMode, uint32 width, uint32 height, uint32 depth, MPTR physAddress)
	{
		using MEM1 = GraphicPack2::TextureRule::FILTER_SETTINGS::MEM1_FILTER;
		if (!filter.format_whitelist.empty() && ListExcludes(filter.format_whitelist, (sint32)format))
			return false;
		if (!filter.format_blacklist.empty() && !ListExcludes(filter.format_blacklist, (sint32)format))
			return false;
		if (!filter.tilemode_whitelist.empty() && ListExcludes(filter.tilemode_whitelist, (sint32)tileMode))
			return false;
		if (!filter.tilemode_blacklist.empty() && !ListExcludes(filter.tilemode_blacklist, (sint32)tileMode))
			return false;
		if (filter.width != -1 && (uint32)filter.width != width)
			return false;
		if (filter.height != -1 && (uint32)filter.height != height)
			return false;
		if (filter.depth != -1 && (uint32)filter.depth != depth)
			return false;
		const bool inMEM1 = mmuRange_MEM1.containsAddress(physAddress);
		if (filter.inMEM1 == MEM1::OUTSIDE && inMEM1)
			return false;
		if (filter.inMEM1 == MEM1::INSIDE && !inMEM1)
			return false;
		return true;
	}

	// number of mip levels until every dimension reached 1 (capped at 20)
	sint32 CountPossibleMips(sint32 w, sint32 h, sint32 d, bool includeDepth)
	{
		for (sint32 i = 0; i < 20; i++)
		{
			if ((w >> i) <= 1 && (h >> i) <= 1 && (!includeDepth || (d >> i) <= 1))
				return i + 1;
		}
		return 1;
	}
}

std::vector<LatteTexture*>& LatteTexture::GetAllTextures()
{
	return AllTextures().textures;
}

LatteTexture::LatteTexture(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
	AllTextures().Insert(this);
	depth = std::max<uint32>(depth, 1);
	this->physAddress = physAddress;
	this->physMipAddress = physMipAddress;
	this->dim = dim;
	this->format = format;
	this->width = width;
	this->height = height;
	this->depth = depth;
	this->pitch = pitch;
	this->mipLevels = mipLevels;
	this->swizzle = swizzle;
	this->tileMode = tileMode;
	this->isDepth = isDepth;
	this->hasStencil = isDepth && FormatHasStencil(format);
	this->lastUpdateEventCounter = LatteTexture_getNextUpdateEventCounter();
	this->lastWriteEventCounter = LatteTexture_getNextUpdateEventCounter();

	// graphic pack texture rules, applied in order; each matching rule starts from the guest size again
	for (const auto& pack : GraphicPack2::GetActiveGraphicPacks())
	{
		for (const auto& rule : pack->GetTextureRules())
		{
			if (!TextureRuleMatches(rule.filter_settings, format, tileMode, width, height, depth, physAddress))
				continue;
			const auto& ow = rule.overwrite_settings;
			overwriteInfo.width = width;
			overwriteInfo.height = height;
			overwriteInfo.depth = depth;
			if (ow.width != -1)
			{
				overwriteInfo.hasResolutionOverwrite = true;
				overwriteInfo.width = ow.width;
			}
			if (ow.height != -1)
			{
				overwriteInfo.hasResolutionOverwrite = true;
				overwriteInfo.height = ow.height;
			}
			if (ow.depth != -1)
			{
				overwriteInfo.hasResolutionOverwrite = true;
				overwriteInfo.depth = ow.depth;
			}
			if (ow.format != -1)
			{
				overwriteInfo.hasFormatOverwrite = true;
				overwriteInfo.format = ow.format;
			}
			if (ow.lod_bias != -1)
			{
				overwriteInfo.hasLodBias = true;
				overwriteInfo.lodBias = ow.lod_bias;
			}
			if (ow.relative_lod_bias != -1)
			{
				overwriteInfo.hasRelativeLodBias = true;
				overwriteInfo.relativeLodBias = ow.relative_lod_bias;
			}
			if (ow.anistropic_value != -1)
				overwriteInfo.anisotropicLevel = ow.anistropic_value;
		}
	}

	// Linear textures are often read by the CPU. Tiny R32F render targets are used
	// for exposure values that the CPU reads back (Cars 3).
	const bool isLinear = tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED;
	const bool isTinyFloatTarget = tileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THIN1 && format == Latte::E_GX2SURFFMT::R32_FLOAT &&
		!isDepth && width == 1 && height <= 8 && depth == 1 && mipLevels == 1;
	if (isLinear || isTinyFloatTarget)
		enableReadback = true;

	const bool resized = overwriteInfo.hasResolutionOverwrite;
	maxPossibleMipLevels = CountPossibleMips(resized ? overwriteInfo.width : (sint32)width, resized ? overwriteInfo.height : (sint32)height, resized ? overwriteInfo.depth : (sint32)depth, dim == Latte::E_DIM::DIM_3D);
}

LatteTexture::~LatteTexture()
{
	AllTextures().Remove(this);
	cemu_assert_debug(baseView == nullptr);
	cemu_assert_debug(views.empty());
}

void LatteTexture_UpdateCacheFromDynamicTextures(LatteTexture* textureDest)
{
	LatteTexture_UpdateTextureFromDynamicChanges(textureDest);
}

void LatteTexture_MarkConnectedTexturesForReloadFromDynamicTextures(LatteTexture* texture)
{
	for (LatteTextureRelation* rel : texture->list_compatibleRelations)
	{
		LatteTexture* other = texture == rel->baseTexture ? rel->subTexture : rel->baseTexture;
		other->reloadFromDynamicTextures = true;
	}
}

void LatteTexture_TrackTextureGPUWrite(LatteTexture* texture, uint32 slice, uint32 mip, uint64 eventCounter)
{
	LatteTexture_MarkDynamicTextureAsChanged(texture->baseView, slice, mip, eventCounter);
	LatteTC_FlagSliceAsGPUUpdated(texture, slice, mip);
	texture->lastUnflushedRTDrawcallIndex = LatteGPUState.drawCallCounter;
}

// --- deletion guards -------------------------------------------------------------

static LatteTextureDeleteGuard* s_innermostDeleteGuard = nullptr;

LatteTextureDeleteGuard::LatteTextureDeleteGuard(LatteTexture* texture) : m_texture(texture), m_outer(s_innermostDeleteGuard)
{
	s_innermostDeleteGuard = this;
}

LatteTextureDeleteGuard::~LatteTextureDeleteGuard()
{
	cemu_assert_debug(s_innermostDeleteGuard == this);
	s_innermostDeleteGuard = m_outer;
}

void LatteTextureDeleteGuard::NotifyDeletion(LatteTexture* texture)
{
	for (LatteTextureDeleteGuard* guard = s_innermostDeleteGuard; guard; guard = guard->m_outer)
	{
		if (guard->m_texture == texture)
			guard->m_deleted = true;
	}
}
