// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "Cafe/OS/libs/gx2/GX2_Surface.h"
#include <bit>

// Surface layout computation (pitch/height/slice padding, alignments, sizes
// and the tile mode each mip level ends up with).
//
// Tiling recap for this memory configuration:
//  - a micro tile is 8x8 elements (8x8x4 for THICK modes)
//  - a macro tile holds one micro tile per pipe/bank pair (2 pipes x 4 banks),
//    arranged as 4x2, 2x4 or 1x8 micro tiles for THIN1, THIN2 and THIN4
//  - addresses interleave the pipe every 256 bytes and the bank above that

using namespace Latte;

namespace LatteAddrLib
{
	namespace
	{
		using HWTM = E_HWTILEMODE;

		uint32 RoundUpPow2(uint32 v)
		{
			return std::bit_ceil<uint32>(v);
		}

		// align to a power-of-two boundary
		uint32 AlignPow2(uint32 v, uint32 alignment)
		{
			return (v + alignment - 1) & ~(alignment - 1);
		}

		// align to any boundary
		uint32 AlignAny(uint32 v, uint32 alignment)
		{
			if ((alignment & (alignment - 1)) == 0)
				return AlignPow2(v, alignment);
			return (v + alignment - 1) / alignment * alignment;
		}

		bool IsMacroMode(HWTM m)
		{
			return static_cast<uint32>(m) >= static_cast<uint32>(HWTM::TM_2D_TILED_THIN1);
		}

		// number of micro tiles stacked vertically in a macro tile (1, 2 or 4)
		uint32 MacroAspect(HWTM m)
		{
			switch (m)
			{
			case HWTM::TM_2D_TILED_THIN2:
			case HWTM::TM_2B_TILED_THIN2:
				return 2;
			case HWTM::TM_2D_TILED_THIN4:
			case HWTM::TM_2B_TILED_THIN4:
				return 4;
			default:
				return 1;
			}
		}

		HWTM WithoutBankSwap(HWTM m)
		{
			switch (m)
			{
			case HWTM::TM_2B_TILED_THIN1: return HWTM::TM_2D_TILED_THIN1;
			case HWTM::TM_2B_TILED_THIN2: return HWTM::TM_2D_TILED_THIN2;
			case HWTM::TM_2B_TILED_THIN4: return HWTM::TM_2D_TILED_THIN4;
			case HWTM::TM_2B_TILED_THICK: return HWTM::TM_2D_TILED_THICK;
			case HWTM::TM_3B_TILED_THIN1: return HWTM::TM_3D_TILED_THIN1;
			case HWTM::TM_3B_TILED_THICK: return HWTM::TM_3D_TILED_THICK;
			default: return m;
			}
		}

		// THICK variant -> THIN1 variant of the same family
		HWTM ThinVariant(HWTM m)
		{
			switch (m)
			{
			case HWTM::TM_1D_TILED_THICK: return HWTM::TM_1D_TILED_THIN1;
			case HWTM::TM_2D_TILED_THICK: return HWTM::TM_2D_TILED_THIN1;
			case HWTM::TM_2B_TILED_THICK: return HWTM::TM_2B_TILED_THIN1;
			case HWTM::TM_3D_TILED_THICK: return HWTM::TM_3D_TILED_THIN1;
			case HWTM::TM_3B_TILED_THICK: return HWTM::TM_3B_TILED_THIN1;
			default: return m;
			}
		}

		// bits per element and block size of a hardware format
		// (0 bits = no layout information for this format)
		struct FormatInfo
		{
			uint32 bits;
			uint32 blockDim; // 4 for BCn, otherwise 1
		};

		FormatInfo GetFormatInfo(E_HWFMT format)
		{
			switch (format)
			{
			case E_HWFMT::HWFMT_8:
			case E_HWFMT::HWFMT_4_4:
			case E_HWFMT::HWFMT_3_3_2:
				return { 8, 1 };
			case E_HWFMT::HWFMT_16:
			case E_HWFMT::HWFMT_16_FLOAT:
			case E_HWFMT::HWFMT_8_8:
			case E_HWFMT::HWFMT_5_6_5:
			case E_HWFMT::HWFMT_6_5_5:
			case E_HWFMT::HWFMT_1_5_5_5:
			case E_HWFMT::HWFMT_4_4_4_4:
			case E_HWFMT::HWFMT_5_5_5_1:
				return { 16, 1 };
			case E_HWFMT::HWFMT_32:
			case E_HWFMT::HWFMT_32_FLOAT:
			case E_HWFMT::HWFMT_16_16:
			case E_HWFMT::HWFMT_16_16_FLOAT:
			case E_HWFMT::HWFMT_8_24:
			case E_HWFMT::HWFMT_8_24_FLOAT:
			case E_HWFMT::HWFMT_24_8:
			case E_HWFMT::HWFMT_24_8_FLOAT:
			case E_HWFMT::HWFMT_10_11_11:
			case E_HWFMT::HWFMT_10_11_11_FLOAT:
			case E_HWFMT::HWFMT_11_11_10:
			case E_HWFMT::HWFMT_11_11_10_FLOAT:
			case E_HWFMT::HWFMT_2_10_10_10:
			case E_HWFMT::HWFMT_8_8_8_8:
			case E_HWFMT::HWFMT_10_10_10_2:
				return { 32, 1 };
			case E_HWFMT::HWFMT_X24_8_32_FLOAT:
			case E_HWFMT::HWFMT_32_32:
			case E_HWFMT::HWFMT_32_32_FLOAT:
			case E_HWFMT::HWFMT_16_16_16_16:
			case E_HWFMT::HWFMT_16_16_16_16_FLOAT:
				return { 64, 1 };
			case E_HWFMT::HWFMT_32_32_32_32:
			case E_HWFMT::HWFMT_32_32_32_32_FLOAT:
				return { 128, 1 };
			case E_HWFMT::HWFMT_BC1:
			case E_HWFMT::HWFMT_BC4:
				return { 64, 4 };
			case E_HWFMT::HWFMT_BC2:
			case E_HWFMT::HWFMT_BC3:
			case E_HWFMT::HWFMT_BC5:
				return { 128, 4 };
			default:
				return { 0, 1 };
			}
		}

		// a surface level to lay out
		struct LevelRequest
		{
			HWTM requestedMode; // tile mode of the surface
			uint32 bits; // bits per element
			uint32 samples;
			uint32 width; // elements
			uint32 height; // elements
			uint32 slices;
			uint32 level;
			bool isCube;
			bool isDepth;
			bool isDisplay;
		};

		// layout of the level
		struct LevelLayout
		{
			HWTM mode;
			uint32 pitch;
			uint32 height;
			uint32 slices;
			uint64 byteSize;
			uint32 baseAlign;
			uint32 pitchAlign;
			uint32 heightAlign;
			uint32 sliceAlign;
		};

		// number of sample groups a micro tile is split into
		uint32 MicroTileSplitCount(HWTM mode, uint32 bits, uint32 samples)
		{
			const uint32 bytesPerSampleTile = bits * 64 / 8;
			if (TM_IsThick(mode))
				samples = 4;
			if (bytesPerSampleTile == 0)
				return 1;
			const uint32 samplesPerSplit = kSplitSize / bytesPerSampleTile;
			if (samplesPerSplit == 0)
				return 1;
			return std::max<uint32>(samples / samplesPerSplit, 1);
		}

		// adjustments that only depend on the tile mode, bit depth and sample count
		HWTM RestrictTileMode(HWTM mode, uint32 bits, uint32 samples, bool isDepth)
		{
			switch (mode)
			{
			case HWTM::TM_1D_TILED_THIN1:
				// MSAA surfaces cannot be micro tiled
				return samples > 1 ? HWTM::TM_2D_TILED_THIN1 : mode;
			case HWTM::TM_1D_TILED_THICK:
				if (samples == 2 || samples == 4)
					return HWTM::TM_2D_TILED_THICK;
				if (samples > 1 || isDepth)
					return HWTM::TM_1D_TILED_THIN1;
				return mode;
			case HWTM::TM_2D_TILED_THICK:
			case HWTM::TM_2B_TILED_THICK:
			case HWTM::TM_3D_TILED_THICK:
			case HWTM::TM_3B_TILED_THICK:
				if (samples > 1 || isDepth || MicroTileSplitCount(mode, bits, samples) > 1)
					return ThinVariant(mode);
				return mode;
			default:
				return mode;
			}
		}

		// tile mode a mip level actually uses: small levels fall back to micro tiling
		HWTM SelectLevelTileMode(const LevelRequest& req)
		{
			HWTM mode = RestrictTileMode(req.requestedMode, req.bits, req.samples, req.isDepth);
			if (req.level == 0)
				return mode;
			const uint32 width = RoundUpPow2(req.width);
			const uint32 height = RoundUpPow2(req.height);
			const uint32 slices = RoundUpPow2(req.slices);
			mode = WithoutBankSwap(mode);
			// a macro tile row must be at least one pipe interleave wide
			const uint32 microTileBytes = (req.samples * req.bits * (TM_GetThickness(mode) * 64) + 7) / 8;
			const uint32 widthFactor = (microTileBytes >= kPipeInterleaveBytes || microTileBytes == 0) ? 1 : kPipeInterleaveBytes / microTileBytes;
			if (IsMacroMode(mode))
			{
				const uint32 aspect = MacroAspect(mode);
				const uint32 macroWidth = 8 * kNumBanks / aspect;
				const uint32 macroHeight = 8 * kNumPipes * aspect;
				if (width < widthFactor * macroWidth || height < macroHeight)
					mode = TM_IsThick(mode) ? HWTM::TM_1D_TILED_THICK : HWTM::TM_1D_TILED_THIN1;
			}
			// thick tiles need at least 4 slices
			if (slices < 4 && (mode == HWTM::TM_1D_TILED_THICK || mode == HWTM::TM_2D_TILED_THICK || mode == HWTM::TM_3D_TILED_THICK))
				mode = ThinVariant(mode);
			// the restrictions are applied again to the final choice
			return RestrictTileMode(mode, req.bits, req.samples, req.isDepth);
		}

		// pads the dimensions to the alignments
		// padDims: 0/3 = pitch, height and slices, 2 = pitch and height only
		void PadToAlignment(HWTM mode, uint32 padDims, bool isCube, LevelLayout& lay)
		{
			if (padDims == 0)
				padDims = 3;
			lay.pitch = AlignAny(lay.pitch, lay.pitchAlign);
			if (padDims >= 2)
				lay.height = AlignPow2(lay.height, lay.heightAlign);
			const bool thick = TM_IsThick(mode);
			if (padDims >= 3 || thick)
			{
				if (isCube)
					lay.slices = RoundUpPow2(lay.slices);
				if (thick)
					lay.slices = AlignPow2(lay.slices, lay.sliceAlign);
			}
		}

		// mip levels > 0 start from power-of-two dimensions; cube maps keep
		// their face count and skip slice padding when they have one face
		void PrepareLevelDims(const LevelRequest& req, LevelLayout& lay, uint32& padDims)
		{
			lay.pitch = req.width;
			lay.height = req.height;
			lay.slices = req.slices;
			if (req.level == 0)
				return;
			lay.pitch = RoundUpPow2(req.width);
			lay.height = RoundUpPow2(req.height);
			if (req.isCube)
				padDims = req.slices <= 1 ? 2 : 0;
			else
				lay.slices = RoundUpPow2(req.slices);
		}

		uint32 DisplayPitchAlign(const LevelRequest& req, uint32 pitchAlign)
		{
			return req.isDisplay ? AlignPow2(pitchAlign, 32) : pitchAlign;
		}

		void LayoutLinear(HWTM mode, const LevelRequest& req, uint32 padDims, LevelLayout& lay)
		{
			if (mode == HWTM::TM_LINEAR_ALIGNED)
			{
				lay.baseAlign = kPipeInterleaveBytes;
				lay.pitchAlign = std::max<uint32>(64, kPipeInterleaveBytes * 8 / req.bits);
			}
			else
			{
				lay.baseAlign = 1;
				lay.pitchAlign = 1;
			}
			lay.pitchAlign = DisplayPitchAlign(req, lay.pitchAlign);
			lay.heightAlign = 1;
			lay.sliceAlign = 1;
			PrepareLevelDims(req, lay, padDims);
			PadToAlignment(mode, padDims, req.isCube, lay);
			lay.mode = mode;
			// note: the sample count is applied twice for linear surfaces
			const uint64 sliceCount = lay.slices * req.samples;
			lay.byteSize = ((uint64)lay.height * lay.pitch * sliceCount * req.bits * req.samples + 7) / 8;
		}

		void LayoutMicroTiled(HWTM mode, const LevelRequest& req, uint32 padDims, LevelLayout& lay)
		{
			PrepareLevelDims(req, lay, padDims);
			if (req.level != 0 && mode == HWTM::TM_1D_TILED_THICK && lay.slices < 4)
				mode = HWTM::TM_1D_TILED_THIN1;
			const uint32 thickness = TM_GetThickness(mode);
			lay.baseAlign = kPipeInterleaveBytes;
			lay.pitchAlign = DisplayPitchAlign(req, std::max<uint32>(8, kPipeInterleaveBytes / req.bits / req.samples / thickness));
			lay.heightAlign = 8;
			lay.sliceAlign = thickness;
			PadToAlignment(mode, padDims, req.isCube, lay);
			lay.mode = mode;
			lay.byteSize = ((uint64)lay.height * lay.pitch * lay.slices * req.bits * req.samples + 7) / 8;
		}

		struct MacroAlignment
		{
			uint32 base;
			uint32 pitch;
			uint32 height;
		};

		MacroAlignment GetMacroAlignment(HWTM mode, uint32 bits, uint32 samples)
		{
			const uint32 aspect = MacroAspect(mode);
			const uint32 thickness = TM_GetThickness(mode);
			const uint32 macroWidth = 8 * kNumBanks / aspect;
			const uint32 macroHeight = 8 * kNumPipes * aspect;
			MacroAlignment a;
			// a macro tile row has to cover at least one pipe interleave
			a.pitch = std::max(macroWidth, macroWidth * (kPipeInterleaveBytes / bits / (8 * thickness) / samples));
			a.height = macroHeight;
			const uint32 macroTileBytes = samples * ((bits * macroHeight * macroWidth + 7) / 8);
			const uint32 alignedRowBytes = ((thickness == 1 ? samples : 4) * a.height * bits * a.pitch + 7) / 8;
			a.base = std::max(thickness == 1 ? macroTileBytes : kPipeInterleaveBytes, alignedRowBytes);
			const uint32 microTileBytes = (thickness * samples * bits * 64 + 7) / 8;
			if (microTileBytes >= kSplitSize)
				a.base /= microTileBytes / kSplitSize;
			return a;
		}

		void LayoutMacroTiled(HWTM mode, const LevelRequest& req, uint32 padDims, LevelLayout& lay)
		{
			PrepareLevelDims(req, lay, padDims);
			HWTM levelMode = mode;
			uint32 sliceAlign = TM_GetThickness(mode);
			if (req.level != 0 && levelMode == HWTM::TM_2D_TILED_THICK && lay.slices < 4)
			{
				levelMode = HWTM::TM_2D_TILED_THIN1;
				sliceAlign = 1;
			}
			// a level that lost its THICK mode must still fit the THICK macro tile, otherwise it is micro tiled
			if (req.level != 0 && mode != req.requestedMode && TM_IsThickAndMacroTiled(req.requestedMode) && !TM_IsThickAndMacroTiled(mode))
			{
				const MacroAlignment thickAlign = GetMacroAlignment(req.requestedMode, req.bits, req.samples);
				const uint32 minPitch = thickAlign.pitch * std::max<uint32>(kPipeInterleaveBytes / 8 / req.bits, 1);
				if (lay.pitch < minPitch || lay.height < thickAlign.height)
				{
					LayoutMicroTiled(HWTM::TM_1D_TILED_THIN1, req, padDims, lay);
					return;
				}
			}
			const MacroAlignment align = GetMacroAlignment(mode, req.bits, req.samples);
			lay.baseAlign = align.base;
			lay.pitchAlign = std::max(align.pitch, ComputeSurfaceBankSwappedWidth(mode, req.bits, req.samples, req.width));
			lay.heightAlign = align.height;
			lay.sliceAlign = sliceAlign;
			PadToAlignment(mode, padDims, req.isCube, lay);
			lay.mode = levelMode;
			lay.byteSize = ((uint64)lay.height * lay.pitch * lay.slices * req.bits * req.samples + 7) / 8;
		}

		void FillTileMaxFields(AddrSurfaceInfo_OUT* out)
		{
			out->pitchTileMax = (out->pitch >> 3) - 1;
			out->heightTileMax = (out->height >> 3) - 1;
			out->sliceTileMax = ((out->height * out->pitch) >> 6) - 1;
		}

		uint32 SliceSizeFromTotal(uint64 surfSize, uint32 depth, bool is3D)
		{
			if (is3D)
				return (uint32)surfSize;
			if (depth == 0)
				return 0; // empty array surface
			return (uint32)(surfSize / depth);
		}

		// GX2's own linear layout without any alignment
		void CalculateLinearSpecial(E_HWFMT hwFormat, uint32 surfaceWidth, uint32 surfaceHeight, uint32 surfaceDepth, E_DIM dim, uint32 surfaceAA, uint32 level, AddrSurfaceInfo_OUT* out)
		{
			const uint32 samples = 1u << surfaceAA;
			const uint32 block = IsCompressedFormat(hwFormat) ? 4 : 1;
			const uint32 levelWidth = surfaceWidth >> level;
			const uint32 levelHeight = std::max<uint32>(surfaceHeight >> level, 1);
			out->bpp = GetFormatBits(hwFormat);
			out->pixelBits = out->bpp;
			out->baseAlign = 1;
			out->pitchAlign = 1;
			out->heightAlign = 1;
			out->depthAlign = 1;
			uint32 height = 0;
			uint32 depth = 0;
			switch (dim)
			{
			case E_DIM::DIM_1D:
				height = 1;
				depth = 1;
				break;
			case E_DIM::DIM_2D:
				height = levelHeight;
				depth = 1;
				break;
			case E_DIM::DIM_3D:
				height = levelHeight;
				depth = std::max<uint32>(surfaceDepth >> level, 1);
				break;
			case E_DIM::DIM_CUBEMAP:
				height = levelHeight;
				depth = std::max<uint32>(surfaceDepth, 6);
				break;
			case E_DIM::DIM_1D_ARRAY:
				height = 1;
				depth = surfaceDepth;
				break;
			case E_DIM::DIM_2D_ARRAY:
				height = levelHeight;
				depth = surfaceDepth;
				break;
			default:
				break;
			}
			out->depth = depth;
			out->pitch = std::max<uint32>(AlignPow2(levelWidth, block) / block, 1);
			out->height = std::max<uint32>(AlignPow2(height, block) / block, 1);
			out->pixelPitch = std::max<uint32>(AlignPow2(levelWidth, block), block);
			out->pixelHeight = std::max<uint32>(AlignPow2(surfaceHeight >> level, block), block);
			// computed in 32 bits
			const uint32 sizeBytes = (out->bpp * samples * out->depth * out->height * out->pitch) >> 3;
			out->surfSize = sizeBytes;
			out->sliceSize = SliceSizeFromTotal(out->surfSize, out->depth, dim == E_DIM::DIM_3D);
			FillTileMaxFields(out);
		}
	}

	uint32 ComputeSurfaceRotationFromTileMode(E_HWTILEMODE tileMode)
	{
		const uint32 m = static_cast<uint32>(tileMode);
		if (!IsMacroMode(tileMode) || m > 15)
			return 0;
		if (m <= static_cast<uint32>(E_HWTILEMODE::TM_2B_TILED_THICK))
			return kNumPipes * (kNumBanks / 2 - 1); // 2D/2B: rotate banks per slice
		return kNumPipes >= 4 ? kNumPipes / 2 - 1 : 1; // 3D/3B: rotate pipes per slice
	}

	// width (in elements) after which bank swapped modes swap banks
	uint32 ComputeSurfaceBankSwappedWidth(E_HWTILEMODE tileMode, uint32 bpp, uint32 numSamples, uint32 pitch)
	{
		if (!TM_IsBankSwapped(tileMode) || bpp == 0)
			return 0;
		const uint32 sampleTileBytes = 8 * bpp;
		uint32 tileSlices = 1;
		const uint32 samplesPerSplit = kSplitSize / sampleTileBytes;
		if (samplesPerSplit != 0)
			tileSlices = std::max<uint32>(numSamples / samplesPerSplit, 1);
		if (TM_IsThickAndMacroTiled(tileMode))
			numSamples = 4;
		const uint32 tileSliceBytes = numSamples * sampleTileBytes / tileSlices;
		const uint32 macroRowBytes = numSamples * MacroAspect(tileMode) * kNumPipes * bpp / tileSlices;
		const uint32 swapMax = kNumPipes * kNumBanks * kRowSize / macroRowBytes;
		const uint32 swapMin = kPipeInterleaveBytes * 8 * kNumBanks / tileSliceBytes;
		const uint32 swapWidth = std::max<uint32>(kSwapSize / 2 / bpp, 1) * 8 * kNumBanks;
		uint32 width = (swapMax >= swapWidth) ? std::max(swapWidth, swapMin) : swapMax;
		while (width != 0 && width >= 2 * pitch)
			width >>= 1;
		return width;
	}

	void GX2CalculateSurfaceInfo(E_GX2SURFFMT surfaceFormat, uint32 surfaceWidth, uint32 surfaceHeight, uint32 surfaceDepth, E_DIM surfaceDim, E_GX2TILEMODE surfaceTileMode, uint32 surfaceAA, uint32 level, AddrSurfaceInfo_OUT* pSurfOut, bool optimizeForDepthBuffer, bool optimizeForScanBuffer)
	{
		const E_HWFMT hwFormat = GetHWFormat(surfaceFormat);
		memset(pSurfOut, 0, sizeof(AddrSurfaceInfo_OUT));
		pSurfOut->size = sizeof(AddrSurfaceInfo_OUT);
		if (surfaceTileMode == E_GX2TILEMODE::TM_LINEAR_SPECIAL)
		{
			CalculateLinearSpecial(hwFormat, surfaceWidth, surfaceHeight, surfaceDepth, surfaceDim, surfaceAA, level, pSurfOut);
			return;
		}

		LevelRequest req{};
		req.requestedMode = MakeHWTileMode(surfaceTileMode); // out of range modes keep their low 4 bits
		req.samples = 1u << surfaceAA;
		req.level = level;
		req.isDepth = optimizeForDepthBuffer;
		req.isDisplay = optimizeForScanBuffer;
		req.width = std::max<uint32>(surfaceWidth >> level, 1);
		const uint32 levelHeight = std::max<uint32>(surfaceHeight >> level, 1);
		bool is3D = false;
		switch (surfaceDim)
		{
		case E_DIM::DIM_1D:
			req.height = 1;
			req.slices = 1;
			break;
		case E_DIM::DIM_2D:
		case E_DIM::DIM_2D_MSAA:
			req.height = levelHeight;
			req.slices = 1;
			break;
		case E_DIM::DIM_3D:
			req.height = levelHeight;
			req.slices = std::max<uint32>(surfaceDepth >> level, 1);
			is3D = true;
			break;
		case E_DIM::DIM_CUBEMAP:
			req.height = levelHeight;
			req.slices = std::max<uint32>(surfaceDepth, 6);
			req.isCube = true;
			break;
		case E_DIM::DIM_1D_ARRAY:
			req.height = 1;
			req.slices = surfaceDepth;
			break;
		case E_DIM::DIM_2D_ARRAY:
		case E_DIM::DIM_2D_ARRAY_MSAA:
			req.height = levelHeight;
			req.slices = surfaceDepth;
			break;
		default:
			req.height = 0;
			req.slices = 0;
			break;
		}

		pSurfOut->pixelBits = GetFormatBits(hwFormat);
		if (hwFormat == E_HWFMT::INVALID_FORMAT)
			return; // nothing to lay out

		// block compressed formats: level 0 is padded to whole blocks, smaller
		// levels use power-of-two pixel sizes; both are then counted in blocks
		const FormatInfo fmt = GetFormatInfo(hwFormat);
		req.bits = fmt.bits;
		if (fmt.blockDim > 1)
		{
			uint32 w, h;
			if (level == 0)
			{
				w = AlignPow2(req.width, 4);
				h = AlignPow2(req.height, 4);
			}
			else
			{
				w = RoundUpPow2(req.width);
				h = RoundUpPow2(req.height);
			}
			req.width = std::max<uint32>(w / fmt.blockDim, 1);
			req.height = std::max<uint32>(h / fmt.blockDim, 1);
		}

		if (req.bits == 0 && req.requestedMode != E_HWTILEMODE::TM_LINEAR_GENERAL)
		{
			// formats without size information can only be laid out linearly
			req.requestedMode = E_HWTILEMODE::TM_LINEAR_GENERAL;
		}

		uint32 padDims = (req.isCube && level == 0) ? 2 : 0;
		const HWTM mode = SelectLevelTileMode(req);
		LevelLayout lay{};
		if (mode == HWTM::TM_LINEAR_GENERAL || mode == HWTM::TM_LINEAR_ALIGNED)
			LayoutLinear(mode, req, padDims, lay);
		else if (mode == HWTM::TM_1D_TILED_THIN1 || mode == HWTM::TM_1D_TILED_THICK)
			LayoutMicroTiled(mode, req, padDims, lay);
		else
			LayoutMacroTiled(mode, req, padDims, lay);

		pSurfOut->pitch = lay.pitch;
		pSurfOut->height = lay.height;
		pSurfOut->depth = lay.slices;
		pSurfOut->surfSize = lay.byteSize;
		pSurfOut->hwTileMode = lay.mode;
		pSurfOut->baseAlign = lay.baseAlign;
		pSurfOut->pitchAlign = lay.pitchAlign;
		pSurfOut->heightAlign = lay.heightAlign;
		pSurfOut->depthAlign = lay.sliceAlign;
		pSurfOut->bpp = req.bits;
		pSurfOut->pixelPitch = std::max<uint32>(lay.pitch * fmt.blockDim, 1);
		pSurfOut->pixelHeight = std::max<uint32>(lay.height * fmt.blockDim, 1);
		pSurfOut->sliceSize = SliceSizeFromTotal(pSurfOut->surfSize, pSurfOut->depth, is3D);
		FillTileMaxFields(pSurfOut);
	}

	namespace
	{
		// walks the mip chain from level 1 to mipIndex and returns the offset of
		// mipIndex relative to the start of level 1; onLevel can adjust the swizzle
		template<typename TLevelHook>
		uint32 WalkMipChain(E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, E_DIM dim, E_HWTILEMODE tileMode, uint32& swizzle, uint32 surfaceAA, sint32 mipIndex, AddrSurfaceInfo_OUT& info, TLevelHook onLevel)
		{
			uint32 offset = 0;
			uint32 previousSize = 0;
			bool previousMacroTiled = TM_IsMacroTiled(tileMode);
			for (sint32 level = 1; level <= mipIndex; level++)
			{
				GX2CalculateSurfaceInfo(format, width, height, depth, dim, MakeGX2TileMode(tileMode), surfaceAA, level, &info);
				onLevel(level, info);
				const bool macroTiled = TM_IsMacroTiled(info.hwTileMode);
				uint32 padding = 0;
				// the first micro tiled level after macro tiled ones is offset by the swizzle
				if (level > 1 && previousMacroTiled && !macroTiled)
					padding = swizzle & 0xFFFF;
				const uint32 align = info.baseAlign != 0 ? info.baseAlign : 1;
				padding += (align - offset % align) % align;
				offset += padding + previousSize;
				previousMacroTiled = macroTiled;
				previousSize = (uint32)info.surfSize;
			}
			return offset;
		}
	}

	uint32 CalculateMipOffset(E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, E_DIM dim, E_HWTILEMODE tileMode, uint32 swizzle, uint32 surfaceAA, sint32 mipIndex)
	{
		cemu_assert_debug(IsValidHWTileMode(tileMode));
		AddrSurfaceInfo_OUT info;
		return WalkMipChain(format, width, height, depth, dim, tileMode, swizzle, surfaceAA, mipIndex, info, [](sint32, const AddrSurfaceInfo_OUT&) {});
	}

	void CalculateMipAndSliceAddr(uint32 physAddr, uint32 physMipAddr, E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, E_DIM dim, E_HWTILEMODE tileMode, uint32 swizzle, uint32 surfaceAA, sint32 mipIndex, sint32 sliceIndex, uint32* outputSliceOffset, uint32* outputSliceSize, sint32* subSliceIndex)
	{
		cemu_assert_debug(IsValidHWTileMode(tileMode));
		AddrSurfaceInfo_OUT info;
		// for macro tiled chains the mip pointer carries the swizzle in bits 8-10
		const uint32 mipOffset = WalkMipChain(format, width, height, depth, dim, tileMode, swizzle, surfaceAA, mipIndex, info,
			[&](sint32 level, const AddrSurfaceInfo_OUT& levelInfo)
			{
				if (level == 1 && TM_IsMacroTiled(levelInfo.hwTileMode))
				{
					swizzle = physMipAddr & 0x700;
					physMipAddr &= ~0x700u;
				}
			});
		if (mipIndex <= 0)
			GX2CalculateSurfaceInfo(format, width, height, depth, dim, MakeGX2TileMode(tileMode), surfaceAA, 0, &info);

		// pitch and height are padded to whole tiles, so the slice size follows directly
		const uint32 sliceBytes = info.pitch * info.height * info.bpp / 8;
		uint32 sliceOffset;
		if (TM_IsThick(info.hwTileMode))
		{
			// four slices share each thick tile
			sliceOffset = (uint32)(sliceIndex & ~3) * sliceBytes;
			*outputSliceSize = sliceBytes * 4;
			*subSliceIndex = sliceIndex & 3;
		}
		else
		{
			sliceOffset = (uint32)sliceIndex * sliceBytes;
			*outputSliceSize = sliceBytes;
			*subSliceIndex = 0;
		}
		sliceOffset += (mipIndex != 0) ? physMipAddr : physAddr;
		*outputSliceOffset = mipOffset + sliceOffset;
	}
};
