// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "GX2.h"
#include "GX2_Texture.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

// Textures and samplers. Like the render state registers, the Latte register setters don't
// mask their inputs, so every word is assembled with its fields set in one fixed order.

namespace GX2
{
	using namespace Latte;
	using TEXWORD4 = LATTE_SQ_TEX_RESOURCE_WORD4_N;
	using SAMPWORD0 = LATTE_SQ_TEX_SAMPLER_WORD0_0;

	/* texture resource words */

	static LATTE_SQ_TEX_RESOURCE_WORD0_N _BuildTexWord0(const GX2Surface& surface, E_HWFMT hwFormat)
	{
		LATTE_SQ_TEX_RESOURCE_WORD0_N w;
		w.set_DIM(surface.dim);
		w.set_TILE_MODE(MakeHWTileMode(surface.tileMode));
		w.set_TILE_TYPE((surface.resFlag & 4) != 0); // depth buffer usage
		// the pitch field counts 8 texel units minus one; block compressed formats store the pitch in blocks
		const uint32 texelPitch = IsCompressedFormat(hwFormat) ? (uint32)surface.pitch * 4 : (uint32)surface.pitch;
		w.set_PITCH(texelPitch != 0 ? (texelPitch >> 3) - 1 : 0x7FF);
		const uint32 width = surface.width;
		w.set_WIDTH(width != 0 ? width - 1 : 0x1FFF);
		return w;
	}

	static LATTE_SQ_TEX_RESOURCE_WORD1_N _BuildTexWord1(const GX2Surface& surface, E_HWFMT hwFormat)
	{
		const E_DIM dim = surface.dim;
		const uint32 depth = surface.depth;
		uint32 depthField = 0;
		if (dim == E_DIM::DIM_CUBEMAP)
			depthField = depth / 6 - 1; // number of cubes
		else if (dim == E_DIM::DIM_3D || dim == E_DIM::DIM_1D_ARRAY || dim == E_DIM::DIM_2D_ARRAY || dim == E_DIM::DIM_2D_ARRAY_MSAA)
			depthField = depth - 1;
		LATTE_SQ_TEX_RESOURCE_WORD1_N w;
		w.set_HEIGHT((uint32)surface.height - 1);
		w.set_DEPTH(depthField);
		w.set_DATA_FORMAT(hwFormat);
		return w;
	}

	static TEXWORD4 _BuildTexWord4(E_GX2SURFFMT format, uint32 compSel, uint32 firstMip)
	{
		const auto comp = HAS_FLAG(format, E_GX2SURFFMT::FMT_BIT_SIGNED) ? TEXWORD4::E_FORMAT_COMP::COMP_SIGNED : TEXWORD4::E_FORMAT_COMP::COMP_UNSIGNED;
		TEXWORD4::E_NUM_FORMAT_ALL numFormat = TEXWORD4::E_NUM_FORMAT_ALL::NUM_FORMAT_NORM;
		if (HAS_FLAG(format, E_GX2SURFFMT::FMT_BIT_FLOAT))
			numFormat = TEXWORD4::E_NUM_FORMAT_ALL::NUM_FORMAT_SCALED;
		else if (HAS_FLAG(format, E_GX2SURFFMT::FMT_BIT_INT))
			numFormat = TEXWORD4::E_NUM_FORMAT_ALL::NUM_FORMAT_INT;
		auto sel = [compSel](uint32 shift) { return (TEXWORD4::E_SEL)((compSel >> shift) & 7); };

		TEXWORD4 w;
		w.set_FORMAT_COMP_X(comp).set_FORMAT_COMP_Y(comp).set_FORMAT_COMP_Z(comp).set_FORMAT_COMP_W(comp);
		w.set_NUM_FORM_ALL(numFormat);
		if (HAS_FLAG(format, E_GX2SURFFMT::FMT_BIT_SRGB))
			w.set_FORCE_DEGAMMA(true);
		w.set_ENDIAN_SWAP(GetSurfaceFormatSwapMode(format));
		w.set_REQUEST_SIZE(2);
		w.set_DST_SEL_X(sel(24)).set_DST_SEL_Y(sel(16)).set_DST_SEL_Z(sel(8)).set_DST_SEL_W(sel(0));
		w.set_BASE_LEVEL(firstMip);
		return w;
	}

	static LATTE_SQ_TEX_RESOURCE_WORD5_N _BuildTexWord5(const GX2Texture* texture)
	{
		const uint32 firstMip = texture->viewFirstMip;
		const uint32 firstSlice = texture->viewFirstSlice;
		const uint32 aa = texture->surface.aa;
		LATTE_SQ_TEX_RESOURCE_WORD5_N w;
		w.set_LAST_LEVEL(firstMip + (uint32)texture->viewNumMips - 1);
		w.set_BASE_ARRAY(firstSlice);
		w.set_LAST_ARRAY(firstSlice + (uint32)texture->viewNumSlices - 1);
		if (texture->surface.dim == E_DIM::DIM_CUBEMAP && (uint32)texture->surface.depth / 6 != 1)
			w.set_UKN_BIT_30(true); // cube map array
		if (aa >= 1 && aa <= 3)
			w.set_LAST_LEVEL(aa); // MSAA textures: holds log2 of the sample count instead
		return w;
	}

	void GX2InitTextureRegs(GX2Texture* texture)
	{
		// zero counts are treated as one
		auto atLeastOne = [](uint32be& v) { if (v == 0) v = 1; };
		atLeastOne(texture->viewNumMips);
		atLeastOne(texture->viewNumSlices);
		atLeastOne(texture->surface.height);
		atLeastOne(texture->surface.depth);
		atLeastOne(texture->surface.numLevels);

		const E_GX2SURFFMT format = texture->surface.format;
		const E_HWFMT hwFormat = GetHWFormat(format);
		texture->regTexWord0 = _BuildTexWord0(texture->surface, hwFormat);
		texture->regTexWord1 = _BuildTexWord1(texture->surface, hwFormat);
		texture->regTexWord4 = _BuildTexWord4(format, texture->compSel, texture->viewFirstMip);
		texture->regTexWord5 = _BuildTexWord5(texture);
		LATTE_SQ_TEX_RESOURCE_WORD6_N w6;
		w6.set_MAX_ANISO(4).set_PERF_MODULATION(7).set_TYPE(LATTE_SQ_TEX_RESOURCE_WORD6_N::E_TYPE::VTX_VALID_TEXTURE);
		texture->regTexWord6 = w6;
	}

	// binds a texture to one of the seven word resource slots that start at firstResourceReg
	static void _BindTexture(GX2Texture* tex, REGADDR firstResourceReg, uint32 slot)
	{
		GX2ReserveCmdSpace(9);
		const GX2Surface& surface = tex->surface;
		MPTR baseAddr = surface.imagePtr;
		MPTR mipAddr = surface.mipPtr != MPTR_NULL ? (MPTR)surface.mipPtr : baseAddr;
		const uint32 swizzle = surface.swizzle;
		cemu_assert_debug((swizzle & 0xFF) == 0);
		if (TM_IsMacroTiled(surface.tileMode))
		{
			// the bank/pipe swizzle is folded into the address of every level that is still macro tiled
			const uint32 stopLevel = (swizzle >> 16) & 0xFF;
			const uint32 swizzleBits = swizzle & 0xFFFF;
			if (stopLevel >= 1)
				baseAddr ^= swizzleBits;
			if (stopLevel >= 2)
				mipAddr ^= swizzleBits;
		}
		const uint32 regOffset = (uint32)firstResourceReg + slot * 7 - mmSQ_TEX_RESOURCE_WORD0;
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_RESOURCE, 8), regOffset,
			tex->regTexWord0, tex->regTexWord1,
			memory_virtualToPhysical(baseAddr) >> 8, memory_virtualToPhysical(mipAddr) >> 8,
			tex->regTexWord4, tex->regTexWord5, tex->regTexWord6);
	}

	void GX2SetPixelTexture(GX2Texture* tex, uint32 texUnit)
	{
		cemu_assert_debug(texUnit < GPU_LIMITS::NUM_TEXTURES_PER_STAGE);
		_BindTexture(tex, REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, texUnit);
	}

	void GX2SetVertexTexture(GX2Texture* tex, uint32 texUnit)
	{
		cemu_assert_debug(texUnit < GPU_LIMITS::NUM_TEXTURES_PER_STAGE);
		_BindTexture(tex, REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS, texUnit);
	}

	void GX2SetGeometryTexture(GX2Texture* tex, uint32 texUnit)
	{
		cemu_assert_debug(texUnit < GPU_LIMITS::NUM_TEXTURES_PER_STAGE);
		_BindTexture(tex, REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS, texUnit);
	}

	void GX2SetComputeTexture(GX2Texture* tex, uint32 texUnit)
	{
		// compute shaders run on the vertex shader stage
		GX2SetVertexTexture(tex, texUnit);
	}

	/* samplers */

	void GX2InitSampler(GX2Sampler* sampler, SAMPWORD0::E_CLAMP clampXYZ, SAMPWORD0::E_XY_FILTER filterMinMag)
	{
		SAMPWORD0 w0{};
		w0.set_CLAMP_X(clampXYZ).set_CLAMP_Y(clampXYZ).set_CLAMP_Z(clampXYZ);
		w0.set_XY_MAG_FILTER(filterMinMag).set_XY_MIN_FILTER(filterMinMag);
		w0.set_Z_FILTER(SAMPWORD0::E_Z_FILTER::POINT).set_MIP_FILTER(SAMPWORD0::E_Z_FILTER::POINT);
		w0.set_TEX_ARRAY_OVERRIDE(true);
		sampler->word0 = w0;
		sampler->word1 = LATTE_SQ_TEX_SAMPLER_WORD1_0().set_MAX_LOD(0x3FF); // min lod 0, no bias
		sampler->word2 = LATTE_SQ_TEX_SAMPLER_WORD2_0().set_TYPE(LATTE_SQ_TEX_SAMPLER_WORD2_0::E_SAMPLER_TYPE::UKN1);
	}

	static SAMPWORD0::E_XY_FILTER _ToAnisotropicFilter(SAMPWORD0::E_XY_FILTER f)
	{
		switch (f)
		{
		case SAMPWORD0::E_XY_FILTER::POINT:
			return SAMPWORD0::E_XY_FILTER::ANISO_POINT;
		case SAMPWORD0::E_XY_FILTER::BILINEAR:
			return SAMPWORD0::E_XY_FILTER::ANISO_BILINEAR;
		default:
			cemu_assert_debug(false);
			return SAMPWORD0::E_XY_FILTER::POINT;
		}
	}

	void GX2InitSamplerXYFilter(GX2Sampler* sampler, SAMPWORD0::E_XY_FILTER magFilter, SAMPWORD0::E_XY_FILTER minFilter, uint32 maxAnisoRatio)
	{
		const bool aniso = maxAnisoRatio != 0;
		SAMPWORD0 w0 = sampler->word0;
		w0.set_XY_MAG_FILTER(aniso ? _ToAnisotropicFilter(magFilter) : magFilter);
		w0.set_XY_MIN_FILTER(aniso ? _ToAnisotropicFilter(minFilter) : minFilter);
		w0.set_MAX_ANISO_RATIO(maxAnisoRatio);
		sampler->word0 = w0;
	}

	void GX2InitSamplerZMFilter(GX2Sampler* sampler, SAMPWORD0::E_Z_FILTER zFilter, SAMPWORD0::E_Z_FILTER mipFilter)
	{
		SAMPWORD0 w0 = sampler->word0;
		w0.set_Z_FILTER(zFilter).set_MIP_FILTER(mipFilter);
		sampler->word0 = w0;
	}

	// float to integer conversions for the LOD fields. Values outside the integer range used to be
	// undefined behaviour; they now produce the same results x86 builds always produced
	// (64-bit truncation for the unsigned fields, INT_MIN for the signed one).
	static uint32 _LodToUnsigned(float v)
	{
		if (!(v > -9.0e18f && v < 9.0e18f))
			return 0;
		return (uint32)(sint64)v;
	}

	static sint32 _LodToSigned(float v)
	{
		if (!(v >= -2147483648.0f && v < 2147483648.0f))
			return std::numeric_limits<sint32>::min();
		return (sint32)v;
	}

	void GX2InitSamplerLOD(GX2Sampler* sampler, float minLod, float maxLod, float lodBias)
	{
		// LODs are 4.6 fixed point (10 bits), the bias is a signed 6.6 value
		// some games pass FLT_MIN/FLT_MAX style extremes, so the range is limited first
		minLod = std::max(minLod, 0.0f);
		maxLod = std::min(maxLod, 16.0f);
		const uint32 minFixed = std::min(_LodToUnsigned(floorf(minLod * 64.0f)), 1023u);
		const uint32 maxFixed = std::min(_LodToUnsigned(floorf(maxLod * 64.0f)), 1023u);
		const sint32 biasFixed = std::clamp(_LodToSigned(floorf(lodBias * 64.0f)), -2048, 2047);
		LATTE_SQ_TEX_SAMPLER_WORD1_0 w1 = sampler->word1;
		w1.set_MIN_LOD(minFixed).set_MAX_LOD(maxFixed).set_LOD_BIAS(biasFixed);
		sampler->word1 = w1;
	}

	void GX2InitSamplerClamping(GX2Sampler* sampler, SAMPWORD0::E_CLAMP clampX, SAMPWORD0::E_CLAMP clampY, SAMPWORD0::E_CLAMP clampZ)
	{
		SAMPWORD0 w0 = sampler->word0;
		w0.set_CLAMP_X(clampX).set_CLAMP_Y(clampY).set_CLAMP_Z(clampZ);
		sampler->word0 = w0;
	}

	void GX2InitSamplerBorderType(GX2Sampler* sampler, SAMPWORD0::E_BORDER_COLOR_TYPE borderColorType)
	{
		SAMPWORD0 w0 = sampler->word0;
		w0.set_BORDER_COLOR_TYPE(borderColorType);
		sampler->word0 = w0;
	}

	void GX2InitSamplerDepthCompare(GX2Sampler* sampler, SAMPWORD0::E_DEPTH_COMPARE depthCompareFunction)
	{
		SAMPWORD0 w0 = sampler->word0;
		w0.set_DEPTH_COMPARE_FUNCTION(depthCompareFunction);
		sampler->word0 = w0;
	}

	// there are 18 sampler slots per shader stage, three registers each
	static void _BindSampler(GX2Sampler* sampler, uint32 globalSamplerIndex)
	{
		GX2ReserveCmdSpace(5);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_SAMPLER, 4), globalSamplerIndex * 3, sampler->word0, sampler->word1, sampler->word2);
	}

	void GX2SetPixelSampler(GX2Sampler* sampler, uint32 samplerIndex)
	{
		_BindSampler(sampler, SAMPLER_BASE_INDEX_PIXEL + samplerIndex);
	}

	void GX2SetVertexSampler(GX2Sampler* sampler, uint32 vertexSamplerIndex)
	{
		_BindSampler(sampler, SAMPLER_BASE_INDEX_VERTEX + vertexSamplerIndex);
	}

	void GX2SetGeometrySampler(GX2Sampler* sampler, uint32 geometrySamplerIndex)
	{
		_BindSampler(sampler, SAMPLER_BASE_INDEX_GEOMETRY + geometrySamplerIndex);
	}

	void GX2SetComputeSampler(GX2Sampler* sampler, uint32 computeSamplerIndex)
	{
		_BindSampler(sampler, SAMPLER_BASE_INDEX_VERTEX + computeSamplerIndex);
	}

	// border colors live in config registers, four per sampler
	static void _SetBorderColor(uint32 firstBorderReg, uint32 samplerIndex, float red, float green, float blue, float alpha)
	{
		GX2ReserveCmdSpace(6);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONFIG_REG, 5), firstBorderReg + samplerIndex * 4 - LATTE_REG_BASE_CONFIG, red, green, blue, alpha);
	}

	void GX2SetPixelSamplerBorderColor(uint32 pixelSamplerIndex, float red, float green, float blue, float alpha)
	{
		_SetBorderColor(REGADDR::TD_PS_SAMPLER0_BORDER_RED, pixelSamplerIndex, red, green, blue, alpha);
	}

	void GX2SetVertexSamplerBorderColor(uint32 vertexSamplerIndex, float red, float green, float blue, float alpha)
	{
		_SetBorderColor(REGADDR::TD_VS_SAMPLER0_BORDER_RED, vertexSamplerIndex, red, green, blue, alpha);
	}

	void GX2SetGeometrySamplerBorderColor(uint32 geometrySamplerIndex, float red, float green, float blue, float alpha)
	{
		_SetBorderColor(REGADDR::TD_GS_SAMPLER0_BORDER_RED, geometrySamplerIndex, red, green, blue, alpha);
	}

	void GX2TextureInit()
	{
		cafeExportRegister("gx2", GX2InitTextureRegs, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPixelTexture, LogType::GX2);
		cafeExportRegister("gx2", GX2SetVertexTexture, LogType::GX2);
		cafeExportRegister("gx2", GX2SetGeometryTexture, LogType::GX2);
		cafeExportRegister("gx2", GX2SetComputeTexture, LogType::GX2);

		cafeExportRegister("gx2", GX2InitSampler, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerXYFilter, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerZMFilter, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerLOD, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerClamping, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerBorderType, LogType::GX2);
		cafeExportRegister("gx2", GX2InitSamplerDepthCompare, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPixelSampler, LogType::GX2);
		cafeExportRegister("gx2", GX2SetVertexSampler, LogType::GX2);
		cafeExportRegister("gx2", GX2SetGeometrySampler, LogType::GX2);
		cafeExportRegister("gx2", GX2SetComputeSampler, LogType::GX2);
		cafeExportRegister("gx2", GX2SetPixelSamplerBorderColor, LogType::GX2);
		cafeExportRegister("gx2", GX2SetVertexSamplerBorderColor, LogType::GX2);
		cafeExportRegister("gx2", GX2SetGeometrySamplerBorderColor, LogType::GX2);
	}
};
