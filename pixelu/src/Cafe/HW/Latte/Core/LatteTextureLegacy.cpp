// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"

#ifdef ENABLE_OPENGL
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureViewGL.h"
#endif

// Texture creation, reloading from guest memory and binding of the textures a draw samples.

// host size / guest size of the texture bound to each unit, read by shaders to rescale coordinates
static float s_textureScale[static_cast<size_t>(LatteConst::ShaderType::TotalCount)][Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE][2] = {};

float* LatteTexture_getEffectiveTextureScale(LatteConst::ShaderType shaderType, sint32 texUnit)
{
	cemu_assert_debug(texUnit >= 0 && texUnit < Latte::GPU_LIMITS::NUM_TEXTURES_PER_STAGE);
	return s_textureScale[static_cast<size_t>(shaderType)][texUnit];
}

static void LatteTexture_setEffectiveTextureScale(LatteConst::ShaderType shaderType, sint32 texUnit, float u, float v)
{
	float* scale = LatteTexture_getEffectiveTextureScale(shaderType, texUnit);
	scale[0] = u;
	scale[1] = v;
}

void LatteTextureLoader_UpdateTextureSliceData(LatteTexture* tex, uint32 sliceIndex, uint32 mipIndex, MPTR physImagePtr, MPTR physMipPtr, Latte::E_DIM dim, uint32 width, uint32 height, uint32 depth, uint32 mipLevels, uint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle, bool dumpTex);

// number of slices uploaded per mip level
static sint32 SlicesToLoad(LatteTexture* tex, sint32 mip)
{
	switch (tex->dim)
	{
	case Latte::E_DIM::DIM_2D_ARRAY:
	case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
		return std::max(tex->depth, 1);
	case Latte::E_DIM::DIM_CUBEMAP:
		cemu_assert_debug((tex->depth % 6) == 0);
		return (tex->depth / 6) * 6; // whole cube faces only
	case Latte::E_DIM::DIM_3D:
		return std::max(tex->depth >> mip, 1);
	default:
		return 1;
	}
}

// uploads every subresource from guest memory
void LatteTexture_ReloadData(LatteTexture* tex)
{
	tex->reloadCount++;
	for (sint32 mip = 0; mip < tex->mipLevels; mip++)
	{
		const sint32 slices = SlicesToLoad(tex, mip);
		for (sint32 slice = 0; slice < slices; slice++)
			LatteTextureLoader_UpdateTextureSliceData(tex, slice, mip, tex->physAddress, tex->physMipAddress, tex->dim, tex->width, tex->height, tex->depth, tex->mipLevels, tex->pitch, tex->tileMode, tex->swizzle, true);
	}
	tex->lastUpdateEventCounter = LatteTexture_getNextUpdateEventCounter();
}

LatteTextureView* LatteTexture_CreateTexture(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth)
{
	LatteTexture* tex = g_renderer->texture_createTextureEx(dim, physAddress, physMipAddress, format, width, height, depth, pitch, mipLevels, swizzle, tileMode, isDepth);
	cemu_assert_debug(mipLevels != 0);
	LatteTexture_InitSliceAndMipInfo(tex);
	LatteTexture_RegisterTextureMemoryOccupancy(tex);
	LatteTexture_ReloadData(tex);
	LatteTC_MarkTextureStillInUse(tex);
	LatteTC_RegisterTexture(tex);
	// the default view spans the whole texture
	tex->baseView = tex->GetOrCreateView(0, tex->mipLevels, 0, tex->depth);
	return tex->baseView;
}

// GX2 surface format from the data format and number format bits of a texture resource
Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N& texUnitWord1, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N& texUnitWord4)
{
	using NumFormat = Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL;
	Latte::E_GX2SURFFMT format = (Latte::E_GX2SURFFMT)texUnitWord1.get_DATA_FORMAT();
	switch (texUnitWord4.get_NUM_FORM_ALL())
	{
	case NumFormat::NUM_FORMAT_SCALED:
		format |= Latte::E_GX2SURFFMT::FMT_BIT_FLOAT;
		break;
	case NumFormat::NUM_FORMAT_INT:
		format |= Latte::E_GX2SURFFMT::FMT_BIT_INT;
		break;
	default:
		break;
	}
	if (texUnitWord4.get_FORCE_DEGAMMA())
		format |= Latte::E_GX2SURFFMT::FMT_BIT_SRGB;
	if (texUnitWord4.get_FORMAT_COMP_X() == Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_FORMAT_COMP::COMP_SIGNED)
		format |= Latte::E_GX2SURFFMT::FMT_BIT_SIGNED;
	return format;
}

namespace
{
	// guest surface described by one texture resource (7 registers)
	struct TextureResourceDesc
	{
		MPTR physAddr;
		MPTR physMipAddr;
		Latte::E_DIM dim;
		uint32 width;
		uint32 height;
		uint32 depth;
		uint32 pitch;
		Latte::E_HWTILEMODE tileMode;
		uint32 swizzle;
		uint32 firstSlice;
		uint32 numSlices;
		uint32 firstMip;
		uint32 numMips;
		Latte::E_GX2SURFFMT format;
	};

	TextureResourceDesc DecodeTextureResource(const _LatteRegisterSetTextureUnit& regs)
	{
		TextureResourceDesc d;
		d.physAddr = regs.word2.get_BASE_ADDRESS() << 8;
		d.physMipAddr = regs.word3.get_MIP_ADDRESS() << 8;
		d.dim = regs.word0.get_DIM();
		d.pitch = (regs.word0.get_PITCH() + 1) << 3;
		d.width = regs.word0.get_WIDTH() + 1;
		d.tileMode = regs.word0.get_TILE_MODE();
		d.height = regs.word1.get_HEIGHT() + 1;
		const uint32 depthField = regs.word1.get_DEPTH();
		switch (d.dim)
		{
		case Latte::E_DIM::DIM_2D_ARRAY:
		case Latte::E_DIM::DIM_3D:
		case Latte::E_DIM::DIM_2D_ARRAY_MSAA:
		case Latte::E_DIM::DIM_1D_ARRAY:
			d.depth = depthField + 1;
			break;
		case Latte::E_DIM::DIM_CUBEMAP:
			d.depth = 6 * (depthField + 1);
			break;
		default:
			d.depth = std::max<uint32>(depthField, 1);
			break;
		}
		if (d.dim == Latte::E_DIM::DIM_1D || d.dim == Latte::E_DIM::DIM_1D_ARRAY)
			d.height = 1;
		// the pitch register counts texels, compressed surfaces use blocks
		if (Latte::IsCompressedFormat(regs.word1.get_DATA_FORMAT()))
			d.pitch /= 4;
		d.firstSlice = regs.word5.get_BASE_ARRAY();
		d.numSlices = regs.word5.get_LAST_ARRAY() + 1 - d.firstSlice;
		d.firstMip = regs.word4.get_BASE_LEVEL();
		d.numMips = regs.word5.get_LAST_LEVEL() + 1 - d.firstMip;
		cemu_assert_debug(d.numMips != 0);
		d.format = LatteTexture_ReconstructGX2Format(regs.word1, regs.word4);
		if (d.dim == Latte::E_DIM::DIM_2D_MSAA)
		{
			// MSAA surfaces have a single level; the level fields may hold garbage (The
			// Mysterious Cities of Gold sets two levels with an invalid mip pointer)
			d.firstMip = 0;
			d.numMips = 1;
		}
		// macro tiled surfaces keep their bank/pipe swizzle in address bits 8-10
		d.swizzle = 0;
		if (Latte::TM_IsMacroTiled(d.tileMode))
		{
			d.swizzle = d.physAddr & 0x700;
			d.physAddr &= ~0x700;
		}
		return d;
	}
}

// looks up or creates the texture of every unit the shader samples and binds it
static void LatteTexture_updateTexturesForStage(LatteDecompilerShader* shaderContext, uint32 glBackendBaseTexUnit, _LatteRegisterSetTextureUnit* texRegBase)
{
	for (sint32 i = 0; i < shaderContext->textureUnitListCount; i++)
	{
		const sint32 unit = shaderContext->textureUnitList[i];
		const TextureResourceDesc d = DecodeTextureResource(texRegBase[unit]);
		if (d.physAddr == MPTR_NULL)
			continue;
		const bool isDepthSampler = shaderContext->textureUsesDepthCompare[unit];
		LatteTextureView* view = isDepthSampler ?
			LatteTextureViewLookupCache::lookupWithColorOrDepthType(d.physAddr, d.width, d.height, d.depth, d.pitch, d.firstMip, d.numMips, d.firstSlice, d.numSlices, d.format, d.dim, true) :
			LatteTextureViewLookupCache::lookup(d.physAddr, d.width, d.height, d.depth, d.pitch, d.firstMip, d.numMips, d.firstSlice, d.numSlices, d.format, d.dim);
		if (!view)
		{
			view = LatteTexture_CreateMapping(d.physAddr, d.physMipAddr, d.width, d.height, d.depth, d.pitch, d.tileMode, d.swizzle, d.firstMip, d.numMips, d.firstSlice, d.numSlices, d.format, d.dim, d.dim, isDepthSampler);
			if (!view)
				continue;
			// the cache changed, previously bound textures may have been replaced
			LatteGPUState.repeatTextureInitialization = true;
		}

#ifdef ENABLE_OPENGL
		if (g_renderer->GetType() == RendererAPI::OpenGL)
		{
			// OpenGL keeps sampler state in the view, so a texture bound to several units
			// in the same draw gets an extra view per binding
			OpenGLRenderer* rendererGL = static_cast<OpenGLRenderer*>(g_renderer.get());
			if (view->lastTextureBindIndex == LatteGPUState.textureBindCounter)
			{
				LatteTextureViewGL* viewGL = (LatteTextureViewGL*)view;
				do
				{
					viewGL = viewGL->GetAlternativeView();
				} while (viewGL->lastTextureBindIndex == LatteGPUState.textureBindCounter);
				view = viewGL;
			}
			view->lastTextureBindIndex = LatteGPUState.textureBindCounter;
			rendererGL->renderstate_updateTextureSettingsGL(shaderContext, view, unit + glBackendBaseTexUnit, texRegBase[unit].word4, unit, isDepthSampler);
		}
#endif

		g_renderer->texture_setLatteTexture(view, unit + glBackendBaseTexUnit);

		LatteTexture* tex = view->baseTexture;
		// A different swizzle or mip chain address means the guest moved the data. A
		// swizzle change that matches the last render target swizzle came from rendering
		// into the texture, whose contents are then still valid.
		bool layoutChanged = false;
		if (tex->swizzle != d.swizzle)
		{
			if (d.swizzle == tex->lastRenderTargetSwizzle)
				tex->swizzle = tex->lastRenderTargetSwizzle;
			else
				layoutChanged = true;
		}
		else if ((d.firstMip + d.numMips) > 1 && tex->physMipAddress != d.physMipAddr)
		{
			cemu_assert_debug(d.physMipAddr != MPTR_NULL);
			layoutChanged = true;
		}
		if (LatteTC_HasTextureChanged(tex) || layoutChanged)
		{
			if (layoutChanged)
			{
				tex->swizzle = d.swizzle;
				if ((d.firstMip + d.numMips) > 1)
					tex->physMipAddress = d.physMipAddr;
			}
			LatteTexture_ReloadData(tex);
		}
		if (tex->reloadFromDynamicTextures)
		{
			LatteTexture_UpdateCacheFromDynamicTextures(tex);
			tex->reloadFromDynamicTextures = false;
		}
		LatteTC_MarkTextureStillInUse(tex);

		// sampling a texture that one of the last draws rendered to needs a barrier
		if ((sint32)(LatteGPUState.drawCallCounter - tex->lastUnflushedRTDrawcallIndex) < 2)
		{
			LatteGPUState.requiresTextureBarrier = true;
			tex->lastUnflushedRTDrawcallIndex = 0;
		}

		float scaleU = 1.0f, scaleV = 1.0f;
		if (tex->overwriteInfo.hasResolutionOverwrite)
		{
			scaleU = (float)tex->overwriteInfo.width / (float)tex->width;
			scaleV = (float)tex->overwriteInfo.height / (float)tex->height;
		}
		LatteTexture_setEffectiveTextureScale(shaderContext->shaderType, unit, scaleU, scaleV);
	}
}

// Binds the textures of all active shader stages. Sets repeatTextureInitialization when
// the cache changed in a way that requires another pass, and requiresTextureBarrier when
// a texture is sampled right after being rendered to.
void LatteTexture_updateTextures()
{
	LatteGPUState.textureBindCounter++;
	if (LatteDecompilerShader* ps = LatteSHRC_GetActivePixelShader())
		LatteTexture_updateTexturesForStage(ps, LATTE_CEMU_PS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_PS);
	LatteDecompilerShader* vs = LatteSHRC_GetActiveVertexShader();
	cemu_assert_debug(vs != nullptr);
	LatteTexture_updateTexturesForStage(vs, LATTE_CEMU_VS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_VS);
	if (LatteDecompilerShader* gs = LatteSHRC_GetActiveGeometryShader())
		LatteTexture_updateTexturesForStage(gs, LATTE_CEMU_GS_TEX_UNIT_BASE, LatteGPUState.contextNew.SQ_TEX_START_GS);
}

sint32 LatteTexture_getEffectiveWidth(LatteTexture* texture)
{
	return texture->overwriteInfo.hasResolutionOverwrite ? texture->overwriteInfo.width : texture->width;
}

// true if texture rules scale both textures by the same factor (within 5%)
bool LatteTexture_doesEffectiveRescaleRatioMatch(LatteTexture* texture1, sint32 mipLevel1, LatteTexture* texture2, sint32 mipLevel2)
{
	const double scale1 = (double)LatteTexture_getEffectiveWidth(texture1) / (double)texture1->width;
	const double scale2 = (double)LatteTexture_getEffectiveWidth(texture2) / (double)texture2->width;
	return !(std::abs(1.0 - scale1 / scale2) > 0.05);
}

// converts guest texel coordinates of a mip level to host coordinates
void LatteTexture_scaleToEffectiveSize(LatteTexture* texture, sint32* x, sint32* y, sint32 mipLevel)
{
	if (!texture->overwriteInfo.hasResolutionOverwrite)
		return;
	*x = *x * std::max(1, texture->overwriteInfo.width >> mipLevel) / std::max(1, texture->width >> mipLevel);
	*y = *y * std::max(1, texture->overwriteInfo.height >> mipLevel) / std::max(1, texture->height >> mipLevel);
}

// stamps for ordering texture updates; starts at 1 so 0 means "never"
static uint64 s_nextUpdateEvent = 1;

uint64 LatteTexture_getNextUpdateEventCounter()
{
	return s_nextUpdateEvent++;
}

void LatteTexture_init()
{
}
