// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "Cafe/OS/libs/gx2/GX2_Surface.h"
#include <bit>

// Element addressing: byte offset of an element (x, y, slice, sample) inside
// linear, micro tiled and macro tiled surfaces.

using namespace Latte;

namespace LatteAddrLib
{
	namespace
	{
		// order in which bank swapped modes permute the banks along a row
		constexpr uint32 kBankSwapPattern[4] = { 0, 1, 3, 2 };

		// Element index bits inside a micro tile. Each entry gives the source
		// of index bits 0-5: high nibble = axis (0 = x, 1 = y), low nibble = bit.
		constexpr uint8 X(uint8 b) { return b; }
		constexpr uint8 Y(uint8 b) { return 0x10 | b; }

		constexpr uint8 kLayoutDepth[6] = { X(0), Y(0), X(1), Y(1), X(2), Y(2) };
		constexpr uint8 kLayout8[6] = { X(0), X(1), X(2), Y(1), Y(0), Y(2) };
		constexpr uint8 kLayout16[6] = { X(0), X(1), X(2), Y(0), Y(1), Y(2) };
		constexpr uint8 kLayout32[6] = { X(0), X(1), Y(0), X(2), Y(1), Y(2) }; // also 96 bit and anything unknown
		constexpr uint8 kLayout64[6] = { X(0), Y(0), X(1), X(2), Y(1), Y(2) };
		constexpr uint8 kLayout128[6] = { Y(0), X(0), X(1), X(2), Y(1), Y(2) };

		const uint8* SelectLayout(uint32 bpp, uint32 microTileType)
		{
			if (microTileType != 0)
				return kLayoutDepth;
			switch (bpp)
			{
			case 8: return kLayout8;
			case 16: return kLayout16;
			case 64: return kLayout64;
			case 128: return kLayout128;
			default: return kLayout32;
			}
		}

		uint32 PipeOfCoord(uint32 x, uint32 y)
		{
			return ((x >> 3) ^ (y >> 3)) & 1;
		}

		uint32 BankOfCoord(uint32 x, uint32 y)
		{
			const uint32 bank0 = ((x >> 3) ^ (y >> 5)) & 1;
			const uint32 bank1 = ((x >> 4) ^ (y >> 4)) & 1;
			return bank0 | (bank1 << 1);
		}

		// macro tile size in elements
		void MacroTileDims(E_HWTILEMODE tileMode, uint32& width, uint32& height)
		{
			uint32 aspect = 1;
			switch (tileMode)
			{
			case E_HWTILEMODE::TM_2D_TILED_THIN2:
			case E_HWTILEMODE::TM_2B_TILED_THIN2:
				aspect = 2;
				break;
			case E_HWTILEMODE::TM_2D_TILED_THIN4:
			case E_HWTILEMODE::TM_2B_TILED_THIN4:
				aspect = 4;
				break;
			default:
				break;
			}
			width = 8 * kNumBanks / aspect;
			height = 8 * kNumPipes * aspect;
		}

		// inserts the pipe and bank bits above the 256 byte interleave
		// tileByteOffset is the byte offset of the micro tile in the unswizzled layout
		uint32 ComposeTiledAddress(uint64 tileByteOffset, uint32 inTileByteOffset, uint32 pipe, uint32 bank)
		{
			constexpr uint32 swizzleBits = 3; // 1 pipe bit + 2 bank bits
			const uint32 offset = (uint32)(tileByteOffset >> swizzleBits) + inTileByteOffset;
			const uint32 low = offset & (kPipeInterleaveBytes - 1);
			const uint32 high = offset & ~(kPipeInterleaveBytes - 1);
			return (high << swizzleBits) | low | (pipe << 8) | (bank << 9);
		}

		// splits a bit offset inside a micro tile into sample groups when the
		// tile is larger than the split size
		struct SampleSplit
		{
			uint32 groups = 1; // number of groups
			uint32 group = 0; // group of this element
			uint32 samplesPerGroup;
		};

		SampleSplit SplitSamples(uint32 numSamples, uint32 microTileBits, uint32& bitOffset)
		{
			SampleSplit s;
			s.samplesPerGroup = numSamples;
			const uint32 microTileBytes = microTileBits >> 3;
			if (numSamples > 1 && microTileBytes > kSplitSize)
			{
				// at least one sample per group (a single sample larger than the split size
				// would otherwise make the group count undefined)
				s.samplesPerGroup = std::max<uint32>(kSplitSize / (microTileBytes / numSamples), 1);
				s.groups = numSamples / s.samplesPerGroup;
				const uint32 groupBits = microTileBits / s.groups;
				s.group = bitOffset / groupBits;
				bitOffset %= groupBits;
			}
			return s;
		}

		uint32 RotateBankPipe(uint32 pipe, uint32 bank, uint32 group, uint32 rotationXor, uint32& pipeOut, uint32& bankOut)
		{
			uint32 bankPipe = pipe + kNumPipes * bank;
			bankPipe ^= (kNumPipes * group * (kNumBanks / 2 + 1)) ^ rotationXor;
			bankPipe %= kNumPipes * kNumBanks;
			pipeOut = bankPipe % kNumPipes;
			bankOut = bankPipe / kNumPipes;
			return bankPipe;
		}
	}

	uint32 _ComputePixelIndexWithinMicroTile(uint32 x, uint32 y, uint32 z, uint32 bpp, E_HWTILEMODE tileMode, uint32 microTileType)
	{
		cemu_assert_debug(microTileType == 0 || microTileType == 1);
		const uint8* layout = SelectLayout(bpp, microTileType);
		uint32 index = 0;
		for (uint32 i = 0; i < 6; i++)
		{
			const uint32 coord = (layout[i] & 0x10) ? y : x;
			index |= ((coord >> (layout[i] & 0xF)) & 1) << i;
		}
		if (TM_IsThick(tileMode))
			index |= (z & 3) << 6;
		return index;
	}

	uint32 ComputeSurfaceAddrFromCoordLinear(uint32 x, uint32 y, uint32 slice, uint32 sample, uint32 bpp, uint32 pitch, uint32 height, uint32 numSlices)
	{
		const uint32 sliceIndex = slice + numSlices * sample;
		const uint32 element = x + y * pitch + sliceIndex * height * pitch;
		return element * bpp / 8;
	}

	uint32 ComputeSurfaceAddrFromCoordMicroTiled(uint32 x, uint32 y, uint32 slice, uint32 bpp, uint32 pitch, uint32 height, E_HWTILEMODE tileMode, bool isDepth)
	{
		const uint32 thickness = (tileMode == E_HWTILEMODE::TM_1D_TILED_THICK) ? 4 : 1;
		const uint32 microTileBytes = thickness * ((bpp * 64 + 7) / 8);
		const uint32 tileIndex = (x >> 3) + (pitch >> 3) * (y >> 3);
		const uint32 tileOffset = (uint32)(microTileBytes * (uint64)tileIndex);
		const uint32 sliceBytes = (uint32)(((uint64)height * pitch * thickness * bpp + 7) / 8);
		const uint32 sliceOffset = sliceBytes * (slice / thickness);
		const uint32 element = _ComputePixelIndexWithinMicroTile(x, y, slice, bpp, tileMode, isDepth ? 1 : 0);
		return sliceOffset + tileOffset + ((bpp * element) >> 3);
	}

	uint32 ComputeSurfaceAddrFromCoordMacroTiled(uint32 x, uint32 y, uint32 slice, uint32 sample, uint32 bpp, uint32 pitch, uint32 height, uint32 numSamples, E_HWTILEMODE tileMode, bool isDepth, uint32 pipeSwizzle, uint32 bankSwizzle)
	{
		const uint32 thickness = TM_GetThickness(tileMode);
		const uint32 microTileBits = numSamples * bpp * thickness * 64;
		const uint32 element = _ComputePixelIndexWithinMicroTile(x, y, slice, bpp, tileMode, isDepth ? 1 : 0);

		// depth tiles interleave samples per element, colour tiles store whole sample planes
		uint32 bitOffset;
		if (isDepth)
			bitOffset = numSamples * bpp * element + bpp * sample;
		else
			bitOffset = bpp * element + sample * (microTileBits / numSamples);
		const SampleSplit split = SplitSamples(numSamples, microTileBits, bitOffset);
		const uint32 inTileBytes = bitOffset >> 3;

		const uint32 rotatedSlice = TM_IsThickAndMacroTiled(tileMode) ? (slice >> 2) : slice;
		const uint32 swizzle = pipeSwizzle + kNumPipes * bankSwizzle;
		uint32 pipe, bank;
		RotateBankPipe(PipeOfCoord(x, y), BankOfCoord(x, y), split.group, swizzle + rotatedSlice * ComputeSurfaceRotationFromTileMode(tileMode), pipe, bank);

		const uint64 sliceBytes = ((uint64)height * pitch * thickness * bpp * split.samplesPerGroup + 7) / 8;
		const uint64 sliceOffset = sliceBytes * ((split.group + split.groups * slice) / thickness);

		uint32 macroWidth, macroHeight;
		MacroTileDims(tileMode, macroWidth, macroHeight);
		const uint32 macroTileBytes = (split.samplesPerGroup * thickness * bpp * macroHeight * macroWidth + 7) >> 3;
		const uint32 macroX = x / macroWidth;
		const uint32 macroTileOffset = (macroX + pitch / macroWidth * (y / macroHeight)) * macroTileBytes;

		if (TM_IsBankSwapped(tileMode))
		{
			const uint32 swapWidth = ComputeSurfaceBankSwappedWidth(tileMode, bpp, split.samplesPerGroup, pitch);
			bank ^= kBankSwapPattern[(macroWidth * macroX / swapWidth) & (kNumBanks - 1)];
		}
		return ComposeTiledAddress(macroTileOffset + sliceOffset, inTileBytes, pipe, bank);
	}

	void SetupCachedSurfaceAddrInfo(CachedSurfaceAddrInfo* info, uint32 slice, uint32 sample, uint32 bpp, uint32 pitch, uint32 height, uint32 depth, uint32 numSamples, E_HWTILEMODE tileMode, int isDepth, uint32 pipeSwizzle, uint32 bankSwizzle)
	{
		cemu_assert_debug(sample == 0); // the cached path addresses sample 0 only
		// tiling parameters
		info->tileMode = tileMode;
		info->isDepth = isDepth;
		info->microTileType = isDepth ? 1 : 0;
		info->microTileThickness = TM_GetThickness(tileMode);
		info->pipeSwizzle = pipeSwizzle;
		info->bankSwizzle = bankSwizzle;
		// surface geometry and the addressed slice/sample
		info->pitch = pitch;
		info->height = height;
		info->depth = depth;
		info->bpp = bpp;
		info->numSamples = numSamples;
		info->slice = slice;
		info->sample = sample;

		info->microTileBits = numSamples * bpp * info->microTileThickness * 64;
		info->microTileBytes = info->microTileBits >> 3;
		info->elementBits = isDepth ? numSamples * bpp : bpp;

		MacroTileDims(tileMode, info->macroTileWidth, info->macroTileHeight);
		info->macroTileWidthShift = std::countr_zero(info->macroTileWidth);
		info->macroTileHeightShift = std::countr_zero(info->macroTileHeight);
		info->macroTileBytes = (numSamples * info->microTileThickness * bpp * info->macroTileHeight * info->macroTileWidth + 7) >> 3;
		info->sliceBytes = (uint32)(((uint64)height * pitch * info->microTileThickness * bpp * numSamples + 7) / 8);
		info->bankSwapWidth = TM_IsBankSwapped(tileMode) ? ComputeSurfaceBankSwappedWidth(tileMode, bpp, numSamples, pitch) : 0;

		const uint32 rotatedSlice = TM_IsThickAndMacroTiled(tileMode) ? (slice >> 2) : slice;
		info->bankPipeXor = pipeSwizzle + kNumPipes * bankSwizzle + rotatedSlice * ComputeSurfaceRotationFromTileMode(tileMode);

		for (uint32 z = 0; z < 8; z++)
			for (uint32 y = 0; y < 8; y++)
				for (uint32 x = 0; x < 8; x++)
					info->microTilePixelIndexTable[(z << 6) | (y << 3) | x] = (uint16)_ComputePixelIndexWithinMicroTile(x, y, z, bpp, tileMode, info->microTileType);
	}

	uint32 ComputeSurfaceAddrFromCoordMacroTiledCached(uint32 x, uint32 y, CachedSurfaceAddrInfo* info)
	{
		const uint32 element = info->microTilePixelIndexTable[((info->slice & 7) << 6) | ((y & 7) << 3) | (x & 7)];
		uint32 bitOffset = element * info->elementBits;
		const SampleSplit split = SplitSamples(info->numSamples, info->microTileBits, bitOffset);
		uint32 pipe, bank;
		RotateBankPipe(PipeOfCoord(x, y), BankOfCoord(x, y), split.group, info->bankPipeXor, pipe, bank);
		// slice and macro tile sizes keep the full sample count here
		const uint32 sliceOffset = info->sliceBytes * ((split.group + split.groups * info->slice) / info->microTileThickness);
		const uint32 macroX = x >> info->macroTileWidthShift;
		const uint32 macroTileOffset = (macroX + (info->pitch >> info->macroTileWidthShift) * (y >> info->macroTileHeightShift)) * info->macroTileBytes;
		if (TM_IsBankSwapped(info->tileMode))
			bank ^= kBankSwapPattern[(info->macroTileWidth * macroX / info->bankSwapWidth) & (kNumBanks - 1)];
		return ComposeTiledAddress(macroTileOffset + sliceOffset, bitOffset >> 3, pipe, bank);
	}

	uint32 ComputeSurfaceAddrFromCoordMacroTiledCached_tm04_sample1(uint32 x, uint32 y, CachedSurfaceAddrInfo* info)
	{
		const uint32 element = info->microTilePixelIndexTable[((info->slice & 7) << 6) | ((y & 7) << 3) | (x & 7)];
		const uint32 inTileBytes = (element * info->elementBits) >> 3;
		// one sample, no bank swap: the rotation only flips pipe and bank bits
		const uint32 pipe = PipeOfCoord(x, y) ^ (info->bankPipeXor & 1);
		const uint32 bank = BankOfCoord(x, y) ^ ((info->bankPipeXor >> 1) & 3);
		const uint32 sliceOffset = info->sliceBytes * (info->slice / info->microTileThickness);
		const uint32 macroTileOffset = ((x >> info->macroTileWidthShift) + (info->pitch >> info->macroTileWidthShift) * (y >> info->macroTileHeightShift)) * info->macroTileBytes;
		return ComposeTiledAddress(macroTileOffset + sliceOffset, inTileBytes, pipe, bank);
	}
};
