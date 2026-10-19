// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "GX2.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "Cafe/HW/Latte/Core/LatteTextureLoader.h"

// Tiling apertures give the CPU a linear (untiled) view of one slice of one mip level of a tiled surface.
//
// The console does the address translation in hardware. Here the aperture is a block in the
// tiling aperture address range: allocating one untiles the surface into it, freeing it tiles the
// (possibly modified) data back into the surface.

void LatteTextureLoader_begin(LatteTextureLoaderCtx* textureLoader, uint32 sliceIndex, uint32 mipIndex, MPTR physImagePtr, MPTR physMipPtr, Latte::E_GX2SURFFMT format, Latte::E_DIM dim, uint32 width, uint32 height, uint32 depth, uint32 mipLevels, uint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle);

namespace
{
	struct Aperture
	{
		MPTR addr;            // start of the linear view
		uint32 size;          // bytes reserved for it
		uint32 handle;
		uint32 endianSwapMode; // stored, not applied
		GX2Surface surface;   // snapshot of the surface when the aperture was created
		uint32 slice;
		uint32 mip;
	};

	constexpr sint32 kMaxApertures = 32;
	Aperture s_apertures[kMaxApertures];
	sint32 s_apertureCount = 0;
	std::atomic<uint32> s_nextHandle{ 1 };

	// size of a mip level along one axis (the shift count wraps like the x86 shift instruction)
	uint32 LevelExtent(uint32 base, uint32 mip)
	{
		return std::max<uint32>(base >> (mip & 31), 1);
	}

	// First fit in 4 KiB steps over the aperture range. A candidate range that ends exactly where
	// an existing one begins is treated as overlapping it.
	MPTR ReserveRange(uint32 size)
	{
		uint32 start = 0;
		for (;;)
		{
			start = (start + 0xFFF) & ~0xFFFu;
			if (start + size >= MEMORY_TILINGAPERTURE_AREA_SIZE)
				return MPTR_NULL;
			bool blocked = false;
			uint32 resumeAt = 0xFFFFFFFF;
			for (sint32 i = 0; i < s_apertureCount; i++)
			{
				const uint32 usedBegin = s_apertures[i].addr - MEMORY_TILINGAPERTURE_AREA_ADDR;
				const uint32 usedEnd = usedBegin + s_apertures[i].size;
				if (start < usedEnd && start + size >= usedBegin)
				{
					blocked = true;
					resumeAt = std::min(resumeAt, usedEnd);
				}
			}
			if (!blocked)
				return MEMORY_TILINGAPERTURE_AREA_ADDR + start;
			start = resumeAt;
		}
	}

	// move one texel of 'bytes' size between the linear view and the tiled surface,
	// in 8 byte steps for the 128 bit formats
	template<uint32 Bytes>
	void MoveTexel(uint8* linear, uint8* tiled, bool toSurface)
	{
		constexpr uint32 kChunk = Bytes > 8 ? 8 : Bytes;
		uint8* to = toSurface ? tiled : linear;
		const uint8* from = toSurface ? linear : tiled;
		for (uint32 o = 0; o < Bytes; o += kChunk)
		{
			uint8 tmp[kChunk];
			memcpy(tmp, from + o, kChunk);
			memcpy(to + o, tmp, kChunk);
		}
	}

	struct RetileJob
	{
		uint8* linear;
		uint8* tiled;
		sint32 columns;
		sint32 rows;
		uint32 linearPitch; // in texels
		LatteAddrLib::CachedSurfaceAddrInfo* addr;
		bool toSurface;
	};

	// byte offset of texel (x,y) inside the tiled level
	uint32 TiledOffset(uint32 hwTileMode, sint32 x, sint32 y, LatteAddrLib::CachedSurfaceAddrInfo* a)
	{
		switch (hwTileMode)
		{
		case 0:
		case 1:
			return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, a->slice, 0, a->bpp, a->pitch, a->height, a->depth);
		case 2:
		case 3:
			return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, a->slice, a->bpp, a->pitch, a->height, (Latte::E_HWTILEMODE)a->tileMode, false);
		case 4:
			return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached_tm04_sample1(x, y, a);
		default:
			return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, a);
		}
	}

	template<uint32 Bytes>
	void RetileRows(const RetileJob& job, uint32 hwTileMode)
	{
		for (sint32 y = 0; y < job.rows; y++)
		{
			// 8 bit formats are laid out with a fixed swap pattern inside the linear view:
			// columns are xor'ed with 8 and rows with 2
			uint8* row = job.linear + (uint32)(y * job.linearPitch * Bytes);
			for (sint32 x = 0; x < job.columns; x++)
			{
				uint8* linearTexel;
				if constexpr (Bytes == 1)
					linearTexel = job.linear + (uint32)((x ^ 8) + (y ^ 2) * job.linearPitch);
				else
					linearTexel = row + (size_t)x * Bytes;
				uint8* tiledTexel = job.tiled + TiledOffset(hwTileMode, x, y, job.addr);
				MoveTexel<Bytes>(linearTexel, tiledTexel, job.toSurface);
			}
		}
	}

	template<uint32 Bytes>
	void RetileLevel(const RetileJob& job, uint32 hwTileMode)
	{
		// only the tile modes titles were seen to use through apertures
		switch (hwTileMode)
		{
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 7:
			RetileRows<Bytes>(job, hwTileMode);
			break;
		default:
			cemu_assert_unimplemented();
			break;
		}
	}

	// guest address of the first byte of a mip level, from the snapshot
	bool LevelAddress(const GX2Surface& s, uint32 mip, MPTR& out)
	{
		if (mip == 0)
			out = s.imagePtr;
		else if (mip == 1)
			out = s.mipPtr;
		else if (mip - 1 < std::size(s.mipOffset))
			out = (uint32)s.mipPtr + (uint32)s.mipOffset[mip - 1];
		else
			return false;
		return true;
	}

	void Retile(Aperture& ap, bool toSurface)
	{
		GX2Surface& s = ap.surface;
		LatteAddrLib::AddrSurfaceInfo_OUT info = {};
		GX2::GX2CalculateSurfaceInfo(&s, ap.mip, &info);

		const uint32 pitch = info.pitch;
		const uint32 bpp = info.bpp;
		const Latte::E_HWTILEMODE hwTileMode = info.hwTileMode;
		const uint32 swizzle = s.swizzle;
		const bool compressed = Latte::IsCompressedFormat(s.format);
		const sint32 width = (sint32)LevelExtent(s.width, ap.mip);
		const sint32 height = (sint32)LevelExtent(s.height, ap.mip);
		uint8* linear = memory_getPointerFromVirtualOffset(ap.addr);

		// 32 bit 2D tiled data goes through Latte's optimized texture loader
		if (hwTileMode == Latte::E_HWTILEMODE::TM_2D_TILED_THIN1 && bpp == 32 && !compressed)
		{
			LatteTextureLoaderCtx loader{};
			LatteTextureLoader_begin(&loader, ap.slice, 0, s.imagePtr, s.mipPtr, s.format, s.dim, width, height, std::max<uint32>(info.depth, 1),
				s.numLevels, pitch, hwTileMode, swizzle);
			loader.decodedTexelCountX = pitch;
			loader.decodedTexelCountY = height;
			if (toSurface)
				optimizedDecodeLoops<uint32, 1, true, false>(&loader, linear);
			else
				optimizedDecodeLoops<uint32, 1, false, false>(&loader, linear);
			return;
		}

		// (the optimized path above finds the level itself)
		MPTR levelAddr;
		if (!LevelAddress(s, ap.mip, levelAddr))
		{
			cemuLog_log(LogType::Force, "GX2 tiling aperture: mip level {} is out of range", ap.mip);
			return;
		}

		LatteAddrLib::CachedSurfaceAddrInfo addr = {};
		LatteAddrLib::SetupCachedSurfaceAddrInfo(&addr, ap.slice, 0, bpp, pitch, info.height, info.depth, 1, hwTileMode, false, (swizzle >> 8) & 1, (swizzle >> 9) & 3);

		// compressed formats are moved as whole 4x4 blocks; a partial block at the edge is left out
		const sint32 blockDim = compressed ? 4 : 1;
		RetileJob job;
		job.linear = linear;
		job.tiled = memory_getPointerFromVirtualOffset(levelAddr);
		job.columns = width / blockDim;
		job.rows = height / blockDim;
		job.linearPitch = pitch;
		job.addr = &addr;
		job.toSurface = toSurface;

		const uint32 tm = (uint32)hwTileMode;
		switch (bpp)
		{
		case 8: RetileLevel<1>(job, tm); break;
		case 16: RetileLevel<2>(job, tm); break;
		case 32: RetileLevel<4>(job, tm); break;
		case 64: RetileLevel<8>(job, tm); break;
		case 128: RetileLevel<16>(job, tm); break;
		default:
			cemu_assert_unimplemented();
			break;
		}
	}

	void WriteResult(MPTR handleOut, MPTR addrOut, uint32 handle, MPTR addr)
	{
		memory_writeU32(addrOut, addr);
		memory_writeU32(handleOut, handle);
	}
}

// GX2AllocateTilingApertureEx(surface, mipLevel, slice, endianSwapMode, handleOut, addrOut)
void gx2Export_GX2AllocateTilingApertureEx(PPCInterpreter_t* hCPU)
{
	const MPTR surfaceAddr = hCPU->gpr[3];
	const uint32 mip = hCPU->gpr[4];
	const uint32 slice = hCPU->gpr[5];
	const uint32 endianSwapMode = hCPU->gpr[6];
	const MPTR handleOut = hCPU->gpr[7];
	const MPTR addrOut = hCPU->gpr[8];
	cemuLog_log(LogType::GX2, "GX2AllocateTilingApertureEx(0x{:08x}, {}, {}, {}, 0x{:08x}, 0x{:08x})", surfaceAddr, mip, slice, endianSwapMode, handleOut, addrOut);
	GX2Surface* surface = (GX2Surface*)memory_getPointerFromVirtualOffset(surfaceAddr);
	cemuLog_log(LogType::GX2, "Tiling Tex: {:08x} {}x{} Swizzle: {:08x} tm: {} fmt: {:04x} use: {:02x}", (uint32)surface->imagePtr, (uint32)surface->width, (uint32)surface->height,
		(uint32)surface->swizzle, (uint32)surface->tileMode.value(), (uint32)surface->format.value(), (uint32)surface->resFlag);

	if (s_apertureCount >= kMaxApertures)
	{
		cemuLog_log(LogType::Force, "GX2AllocateTilingApertureEx: all {} apertures are in use", kMaxApertures);
		WriteResult(handleOut, addrOut, 0, MPTR_NULL);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	// room for the whole pitch of the level, with the height rounded up to whole 4 row blocks
	uint32 bitsPerTexel = Latte::GetFormatBits(surface->format);
	if (Latte::IsCompressedFormat(surface->format))
		bitsPerTexel /= 16;
	LatteAddrLib::AddrSurfaceInfo_OUT level0 = {};
	GX2::GX2CalculateSurfaceInfo(surface, 0, &level0);
	const uint32 rows = (LevelExtent(surface->height, mip) + 3) & ~3u;
	const uint32 size = (level0.pitch * rows * bitsPerTexel + 7) / 8;

	// a handle is used up even if no range is left
	const uint32 handle = s_nextHandle.fetch_add(1);
	const MPTR addr = ReserveRange(size);
	if (addr == MPTR_NULL)
	{
		cemu_assert_suspicious();
		WriteResult(handleOut, addrOut, 0, MPTR_NULL);
		osLib_returnFromFunction(hCPU, 0);
		return;
	}

	Aperture& ap = s_apertures[s_apertureCount++];
	ap.addr = addr;
	ap.size = size;
	ap.handle = handle;
	ap.endianSwapMode = endianSwapMode;
	ap.slice = slice;
	ap.mip = mip;
	memcpy(&ap.surface, surface, sizeof(GX2Surface));

	WriteResult(handleOut, addrOut, handle, addr);
	// fill the linear view with the current surface contents
	Retile(ap, false);
	osLib_returnFromFunction(hCPU, 0);
}

void gx2Export_GX2FreeTilingAperture(PPCInterpreter_t* hCPU)
{
	const uint32 handle = hCPU->gpr[3];
	cemuLog_log(LogType::GX2, "GX2FreeTilingAperture(0x{:08x})", handle);
	for (sint32 i = 0; i < s_apertureCount; i++)
	{
		if (s_apertures[i].handle != handle)
			continue;
		// write the linear view back into the surface
		Retile(s_apertures[i], true);
		// keep the table packed by moving the last entry into the gap
		const sint32 last = s_apertureCount - 1;
		if (i != last)
			memcpy(&s_apertures[i], &s_apertures[last], sizeof(Aperture));
		s_apertureCount = last;
		break;
	}
	osLib_returnFromFunction(hCPU, 0);
}
