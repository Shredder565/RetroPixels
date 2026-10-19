// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

// Copy loops between a tiled surface and a linear texel buffer.
// texelBaseType * texelBaseTypeCount is the size of one texel (or one BCn block),
// isEncodeDirection copies linear -> tiled instead of tiled -> linear.

namespace LatteFastDecode
{
	// copies one texel between surface memory and the linear buffer
	template<typename T, int count, bool encode>
	inline void CopyTexel(uint8* surface, T* linear)
	{
		T* s = reinterpret_cast<T*>(surface);
		for (int i = 0; i < count; i++)
		{
			if constexpr (encode)
				s[i] = linear[i];
			else
				linear[i] = s[i];
		}
	}

	// copies n consecutive bytes
	template<bool encode>
	inline void CopyRun(uint8* surface, uint8* linear, size_t n)
	{
		if constexpr (encode)
			memcpy(surface, linear, n);
		else
			memcpy(linear, surface, n);
	}

	// micro tile element index -> byte offset; elements of tiles larger than 256 bytes
	// are spread over the pipe/bank interleave (8 x 256 byte groups)
	template<size_t texelSize>
	inline uint32 InTileOffset(uint32 elementIndex)
	{
		uint32 offset = elementIndex * (uint32)texelSize;
		if constexpr (texelSize * 64 > 256)
			offset = (offset & 0xFF) | ((offset & ~0xFFu) << 3);
		return offset;
	}

	// For 2D_TILED_THIN1 colour surfaces the eight texels of a micro tile row form
	// runs that stay contiguous in memory. Returns the run length in texels
	// (0 = copy texel by texel). Runs start at row elements 0, run, 2*run, ...
	// and are spaced 'stride' elements apart in the micro tile.
	template<size_t texelSize>
	constexpr uint32 RowRunLength() { return texelSize == 1 ? 8 : (texelSize == 4 ? 4 : (texelSize == 8 ? 2 : 0)); }
	template<size_t texelSize>
	constexpr uint32 RowRunStride() { return texelSize == 4 ? 8 : (texelSize == 8 ? 4 : 0); }

	// full 8x8 micro tiles of a 2D_TILED_THIN1 single sample surface
	template<typename T, int count, bool encode, bool useRuns>
	void CopyFullTiles(LatteTextureLoaderCtx* ctx, uint8* linearData, sint32 tilesWidth, sint32 tilesHeight)
	{
		constexpr size_t texelSize = sizeof(T) * count;
		const uint16* sliceTable = ctx->computeAddrInfo.microTilePixelIndexTable + ((ctx->computeAddrInfo.slice & 7) << 6);
		const size_t linearRowBytes = (size_t)ctx->decodedTexelCountX * texelSize;
		for (sint32 ty = 0; ty < tilesHeight; ty += 8)
		{
			for (sint32 tx = 0; tx < tilesWidth; tx += 8)
			{
				const uint32 tileAddr = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached_tm04_sample1(tx, ty, &ctx->computeAddrInfo);
				for (sint32 row = 0; row < 8; row++)
				{
					uint8* linearRow = linearData + (size_t)(ty + row) * linearRowBytes + (size_t)tx * texelSize;
					const uint16* rowTable = sliceTable + row * 8;
					if constexpr (useRuns)
					{
						constexpr uint32 runLength = RowRunLength<texelSize>();
						constexpr uint32 runStride = RowRunStride<texelSize>();
						uint8* rowBase = ctx->inputData + tileAddr + InTileOffset<texelSize>(rowTable[0]);
						for (uint32 r = 0; r < 8 / runLength; r++)
							CopyRun<encode>(rowBase + r * runStride * texelSize, linearRow + r * runLength * texelSize, runLength * texelSize);
					}
					else
					{
						for (sint32 col = 0; col < 8; col++)
							CopyTexel<T, count, encode>(ctx->inputData + tileAddr + InTileOffset<texelSize>(rowTable[col]), reinterpret_cast<T*>(linearRow) + col * count);
					}
				}
			}
		}
	}

	// texels of a 2D_TILED_THIN1 surface addressed one by one
	template<typename T, int count, bool encode>
	void CopyTexelRect(LatteTextureLoaderCtx* ctx, uint8* linearData, sint32 x0, sint32 x1, sint32 y0, sint32 y1)
	{
		constexpr size_t texelSize = sizeof(T) * count;
		for (sint32 y = y0; y < y1; y++)
		{
			T* linear = reinterpret_cast<T*>(linearData + ((size_t)y * ctx->decodedTexelCountX + x0) * texelSize);
			for (sint32 x = x0; x < x1; x++, linear += count)
			{
				const uint32 addr = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached_tm04_sample1(x, y, &ctx->computeAddrInfo);
				CopyTexel<T, count, encode>(ctx->inputData + addr, linear);
			}
		}
	}
};

template<typename texelBaseType, int texelBaseTypeCount, bool isEncodeDirection, bool isCompressed>
void optimizedDecodeLoops(LatteTextureLoaderCtx* textureLoader, uint8* outputData)
{
	using namespace LatteFastDecode;
	using T = texelBaseType;
	constexpr int count = texelBaseTypeCount;
	constexpr bool encode = isEncodeDirection;
	constexpr size_t texelSize = sizeof(T) * count;

	sint32 texelsX = textureLoader->width;
	sint32 texelsY = textureLoader->height;
	if constexpr (isCompressed)
	{
		texelsX = (texelsX + 3) / 4;
		texelsY = (texelsY + 3) / 4;
	}

	if (textureLoader->tileMode == Latte::E_HWTILEMODE::TM_2D_TILED_THIN1 && textureLoader->computeAddrInfo.numSamples == 1)
	{
		const sint32 fullX = texelsX & ~7;
		const sint32 fullY = texelsY & ~7;
		constexpr bool runsPossible = (count == 1) && RowRunLength<texelSize>() != 0;
		if (runsPossible && textureLoader->computeAddrInfo.microTileType == 0)
			CopyFullTiles<T, count, encode, runsPossible>(textureLoader, outputData, fullX, fullY);
		else
			CopyFullTiles<T, count, encode, false>(textureLoader, outputData, fullX, fullY);
		// partial tiles: right edge of the full rows, then the remaining rows
		CopyTexelRect<T, count, encode>(textureLoader, outputData, fullX, texelsX, 0, fullY);
		CopyTexelRect<T, count, encode>(textureLoader, outputData, 0, texelsX, fullY, texelsY);
	}
	else if (textureLoader->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
	{
		const uint32 sliceTexelOffset = textureLoader->sliceIndex * textureLoader->height * textureLoader->pitch;
		for (sint32 y = 0; y < texelsY; y++)
		{
			uint8* linear = outputData + (size_t)y * textureLoader->decodedTexelCountX * texelSize;
			uint8* surface = textureLoader->inputData + (textureLoader->pitch * y + sliceTexelOffset) * texelSize;
			CopyRun<encode>(surface, linear, (size_t)std::max<sint32>(texelsX, 0) * texelSize);
		}
	}
	else
	{
		// any other tile mode goes through the generic per-texel address function
		for (sint32 y = 0; y < textureLoader->height; y += textureLoader->stepY)
		{
			T* linear = reinterpret_cast<T*>(outputData + (size_t)(y / textureLoader->stepY) * textureLoader->decodedTexelCountX * texelSize);
			for (sint32 x = 0; x < textureLoader->width; x += textureLoader->stepX, linear += count)
				CopyTexel<T, count, encode>(LatteTextureLoader_GetInput(textureLoader, x, y), linear);
		}
	}
}
