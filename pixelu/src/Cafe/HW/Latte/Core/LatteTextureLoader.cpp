// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "config/ActiveSettings.h"
#include "Cafe/CafeSystem.h"

// Prepares ctx for reading or writing one slice of one mip level of a guest surface.
void LatteTextureLoader_begin(LatteTextureLoaderCtx* ctx, uint32 sliceIndex, uint32 mipIndex, MPTR physImagePtr, MPTR physMipPtr, Latte::E_GX2SURFFMT format, Latte::E_DIM dim, uint32 width, uint32 height, uint32 depth, uint32 mipLevels, uint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle)
{
	constexpr uint32 aaMode = 0; // multisampled surfaces are not loaded through here

	cemu_assert_debug(mipLevels != 0);
	ctx->physAddress = physImagePtr;
	ctx->physMipAddress = physMipPtr;
	ctx->sliceIndex = sliceIndex;
	ctx->mipLevels = std::max<uint32>(mipLevels, 1);
	ctx->tileMode = tileMode;
	ctx->bpp = Latte::GetFormatBits(format);
	const bool blockCompressed = Latte::IsCompressedFormat(format);
	ctx->stepX = blockCompressed ? 4 : 1;
	ctx->stepY = ctx->stepX;

	// bits 8-10 of the swizzle word hold the pipe (1 bit) and bank (2 bits) swizzle
	auto applySwizzle = [ctx](uint32 swizzleBits)
	{
		ctx->pipeSwizzle = (swizzleBits >> 8) & 1;
		ctx->bankSwizzle = (swizzleBits >> 9) & 3;
	};
	applySwizzle(swizzle);

	// A macro tiled mip chain carries its own swizzle in the low bits of the
	// mip pointer. That only applies when level 1 is still macro tiled.
	if (mipIndex > 0 && Latte::TM_IsMacroTiled(tileMode))
	{
		LatteAddrLib::AddrSurfaceInfo_OUT firstMip;
		LatteAddrLib::GX2CalculateSurfaceInfo(format, width, height, depth, dim, Latte::MakeGX2TileMode(tileMode), aaMode, 1, &firstMip);
		if (Latte::TM_IsMacroTiled(firstMip.hwTileMode))
		{
			applySwizzle(physMipPtr & 0x700);
			physMipPtr &= ~0x700;
			ctx->physMipAddress = physMipPtr;
		}
	}

	LatteAddrLib::AddrSurfaceInfo_OUT levelInfo;
	LatteAddrLib::GX2CalculateSurfaceInfo(format, width, height, depth, dim, Latte::MakeGX2TileMode(tileMode), aaMode, mipIndex, &levelInfo);
	ctx->levelOffset = LatteAddrLib::CalculateMipOffset(format, width, height, depth, dim, tileMode, swizzle, aaMode, mipIndex);
	ctx->tileMode = levelInfo.hwTileMode;
	ctx->minOffsetOutdated = 0;
	ctx->maxOffsetOutdated = (sint32)levelInfo.surfSize;
	ctx->surfaceInfoHeight = levelInfo.height;
	ctx->surfaceInfoDepth = levelInfo.depth;

	// Minecraft (any region) uses linear sprite atlases whose smaller mips keep the
	// application supplied pitch instead of the computed (aligned) one
	const uint64 titleIdAnyRegion = CafeSystem::GetForegroundTitleId() & ~0x300ULL;
	if (tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED && titleIdAnyRegion == 0x000500301001200aULL)
		levelInfo.pitch = std::max<uint32>(pitch >> mipIndex, 1);

	ctx->width = std::max<sint32>((sint32)(width >> mipIndex), 1);
	ctx->height = std::max<sint32>((sint32)(height >> mipIndex), 1);
	ctx->pitch = levelInfo.pitch;

	if (mipIndex == 0)
		ctx->inputData = (uint8*)memory_getPointerFromPhysicalOffset(physImagePtr);
	else
		ctx->inputData = (uint8*)memory_getPointerFromPhysicalOffset(physMipPtr) + ctx->levelOffset;

	LatteAddrLib::SetupCachedSurfaceAddrInfo(&ctx->computeAddrInfo, ctx->sliceIndex, 0, ctx->bpp, ctx->pitch, levelInfo.height, depth, 1, ctx->tileMode, false, ctx->pipeSwizzle, ctx->bankSwizzle);
}

uint8* LatteTextureLoader_GetInput(LatteTextureLoaderCtx* ctx, sint32 x, sint32 y)
{
	const sint32 ex = x / ctx->stepX;
	const sint32 ey = y / ctx->stepY;
	uint32 offset;
	switch (ctx->tileMode)
	{
	case Latte::E_HWTILEMODE::TM_LINEAR_GENERAL:
	case Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED:
		offset = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(ex, ey, ctx->sliceIndex, 0, ctx->bpp, ctx->pitch, ctx->surfaceInfoHeight, ctx->surfaceInfoDepth);
		break;
	case Latte::E_HWTILEMODE::TM_1D_TILED_THIN1:
	case Latte::E_HWTILEMODE::TM_1D_TILED_THICK:
		offset = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(ex, ey, ctx->sliceIndex, ctx->bpp, ctx->pitch, ctx->surfaceInfoHeight, ctx->tileMode, false);
		break;
	default:
		offset = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(ex, ey, &ctx->computeAddrInfo);
		break;
	}
	return ctx->inputData + offset;
}

// address of pixel (x, y) of a linear surface, through the generic linear address function
static uint8* LatteTextureLoader_getInputLinearOptimized(LatteTextureLoaderCtx* ctx, sint32 x, sint32 y)
{
	return ctx->inputData + LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x / ctx->stepX, y / ctx->stepY, ctx->sliceIndex, 0, ctx->bpp, ctx->pitch, ctx->surfaceInfoHeight, ctx->surfaceInfoDepth);
}

// --- BCn block decoding --------------------------------------------------

namespace
{
	// the 48 bit field of 3 bit indices that BC3 alpha, BC4 and BC5 use
	uint64 Read48(const uint8* p)
	{
		uint64 v = 0;
		for (int i = 5; i >= 0; i--)
			v = (v << 8) | p[i];
		return v;
	}

	// 8 entry palette of an interpolated channel (BC3 alpha, BC4/BC5).
	// e0 > e1 selects six blends, otherwise four blends plus 'low' and 1.
	void BuildChannelPalette(float e0, float e1, bool sixBlends, float low, float pal[8])
	{
		pal[0] = e0;
		pal[1] = e1;
		if (sixBlends)
		{
			for (int i = 1; i <= 6; i++)
				pal[1 + i] = ((float)(7 - i) * e0 + (float)i * e1) / 7.0f;
		}
		else
		{
			for (int i = 1; i <= 4; i++)
				pal[1 + i] = ((float)(5 - i) * e0 + (float)i * e1) / 5.0f;
			pal[6] = low;
			pal[7] = 1.0f;
		}
	}

	// writes the 16 values of an interpolated channel to out[i * stride]
	void DecodeChannelBlock(const uint8* block, float e0, float e1, float low, float* out, int stride)
	{
		float pal[8];
		BuildChannelPalette(e0, e1, e0 > e1, low, pal);
		const uint64 indices = Read48(block + 2);
		for (int i = 0; i < 16; i++)
			out[i * stride] = pal[(indices >> (3 * i)) & 7];
	}

	float UnormByte(uint8 v)
	{
		return (float)v / 255.0f;
	}

	// signed byte endpoint: re-biased to 0..1 first, then stretched to -1..1
	float SnormByte(uint8 v)
	{
		const float biased = ((float)(sint8)v + 128.0f) / 255.0f;
		return biased * 2.0f - 1.0f;
	}

	// colour half of BC2/BC3 (always the four colour mode) into RGB of an RGBA block
	void DecodeColorRGB(const uint8* colorBlock, float* rgba)
	{
		LatteTexDec::BC1Palette pal;
		pal.Build(LatteTexDec::Read16(colorBlock), LatteTexDec::Read16(colorBlock + 2), true);
		const uint32 indices = LatteTexDec::Read32(colorBlock + 4);
		for (int i = 0; i < 16; i++)
		{
			const uint32 c = (indices >> (2 * i)) & 3;
			rgba[i * 4 + 0] = pal.r[c];
			rgba[i * 4 + 1] = pal.g[c];
			rgba[i * 4 + 2] = pal.b[c];
		}
	}
}

void decodeBC1Block(uint8* inputData, float* output4x4RGBA)
{
	LatteTexDec::BC1Palette pal;
	pal.BuildBC1(inputData);
	const uint32 indices = LatteTexDec::Read32(inputData + 4);
	for (int i = 0; i < 16; i++)
	{
		const uint32 c = (indices >> (2 * i)) & 3;
		float* t = output4x4RGBA + i * 4;
		t[0] = pal.r[c];
		t[1] = pal.g[c];
		t[2] = pal.b[c];
		t[3] = pal.a[c];
	}
}

void decodeBC2Block_UNORM(uint8* inputData, float* imageRGBA)
{
	DecodeColorRGB(inputData + 8, imageRGBA);
	// explicit 4 bit alpha, low nibble first
	for (int i = 0; i < 16; i++)
	{
		const uint8 nibble = (inputData[i >> 1] >> ((i & 1) * 4)) & 0xF;
		imageRGBA[i * 4 + 3] = (float)LatteTexDec::Widen4(nibble) / 255.0f;
	}
}

void decodeBC3Block_UNORM(uint8* inputData, float* imageRGBA)
{
	DecodeColorRGB(inputData + 8, imageRGBA);
	DecodeChannelBlock(inputData, UnormByte(inputData[0]), UnormByte(inputData[1]), 0.0f, imageRGBA + 3, 4);
}

void decodeBC4Block_UNORM(uint8* blockStorage, float* rOutput)
{
	DecodeChannelBlock(blockStorage, UnormByte(blockStorage[0]), UnormByte(blockStorage[1]), 0.0f, rOutput, 1);
}

void decodeBC5Block_UNORM(uint8* blockStorage, float* rgOutput)
{
	DecodeChannelBlock(blockStorage, UnormByte(blockStorage[0]), UnormByte(blockStorage[1]), 0.0f, rgOutput, 2);
	DecodeChannelBlock(blockStorage + 8, UnormByte(blockStorage[8]), UnormByte(blockStorage[9]), 0.0f, rgOutput + 1, 2);
}

void decodeBC5Block_SNORM(uint8* blockStorage, float* rgOutput)
{
	DecodeChannelBlock(blockStorage, SnormByte(blockStorage[0]), SnormByte(blockStorage[1]), -1.0f, rgOutput, 2);
	DecodeChannelBlock(blockStorage + 8, SnormByte(blockStorage[8]), SnormByte(blockStorage[9]), -1.0f, rgOutput + 1, 2);
}

// --- upload ----------------------------------------------------------------

void LatteTextureLoader_loadTextureDataIntoSlice(LatteTexture* hostTexture, sint32 width, sint32 height, sint32 depth, sint32 mipLevels, void* pixelData, sint32 sliceIndex, sint32 mipIndex, uint32 compressedImageSize)
{
	cemu_assert_debug(mipIndex != 0 || (width == hostTexture->width && height == hostTexture->height && depth == hostTexture->depth));
	cemu_assert_debug(mipLevels == hostTexture->mipLevels);
	// a texture replaced with a different size or format cannot take the guest data as is
	if (hostTexture->overwriteInfo.hasResolutionOverwrite || hostTexture->overwriteInfo.hasFormatOverwrite)
		g_renderer->texture_clearSlice(hostTexture, sliceIndex, mipIndex);
	else
		g_renderer->texture_loadSlice(hostTexture, width, height, depth, pixelData, sliceIndex, mipIndex, compressedImageSize);
}

void LatteTextureLoader_UpdateTextureSliceData(LatteTexture* tex, uint32 sliceIndex, uint32 mipIndex, MPTR physImagePtr, MPTR physMipPtr, Latte::E_DIM dim, uint32 width, uint32 height, uint32 depth, uint32 mipLevels, uint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle, bool dumpTex)
{
	LatteTextureLoaderCtx ctx = {};
	const Latte::E_GX2SURFFMT format = tex->format;
	LatteTextureLoader_begin(&ctx, sliceIndex, mipIndex, physImagePtr, physMipPtr, format, dim, width, height, depth, mipLevels, pitch, tileMode, swizzle);

	TextureDecoder* decoder = g_renderer->texture_chooseDecodedFormat(format, tex->isDepth, dim, width, height);

	if (!tex->isDataDefined)
	{
		tex->AllocateOnHost();
		tex->isDataDefined = true;
		// without a decoder the texture is cleared once, which also moves a Vulkan
		// image out of the undefined layout
		if (!decoder)
		{
			if (tex->isDepth)
				g_renderer->texture_clearDepthSlice(tex, 0, 0, true, tex->hasStencil, 0.0f, 0);
			else
				g_renderer->texture_clearColorSlice(tex, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
		}
	}
	if (!decoder)
		return;

	ctx.dump = ActiveSettings::DumpTexturesEnabled();
	ctx.dumpRGBA = nullptr;
	if (ctx.dump)
		ctx.dumpRGBA = (uint8*)calloc((size_t)ctx.width * ctx.height, 4);

	ctx.decodedTexelCountX = decoder->getTexelCountX(&ctx);
	ctx.decodedTexelCountY = decoder->getTexelCountY(&ctx);
	const uint32 imageSize = decoder->calculateImageSize(&ctx);
	uint8* pixelData = (uint8*)g_renderer->texture_acquireTextureUploadBuffer(imageSize);

	const bool overwritten = tex->overwriteInfo.hasFormatOverwrite || tex->overwriteInfo.hasResolutionOverwrite;
	if (!overwritten)
		decoder->decode(&ctx, pixelData);

	if (ctx.dump)
	{
		uint8* out = ctx.dumpRGBA;
		for (sint32 y = 0; y < ctx.height; y++)
		{
			for (sint32 x = 0; x < ctx.width; x++, out += 4)
				decoder->decodePixelToRGBA(LatteTextureLoader_GetInput(&ctx, x, y), out, (uint8)(x % ctx.stepX), (uint8)(y % ctx.stepY));
		}
	}

	// The tracked range must be updated before the upload, otherwise a guest write
	// that lands while the data is being uploaded could be missed
	if (mipIndex == 0 || (tex->texDataPtrLow == 0 && tex->texDataPtrHigh == 0))
	{
		tex->texDataPtrLow = physImagePtr + ctx.minOffsetOutdated;
		tex->texDataPtrHigh = physImagePtr + ctx.maxOffsetOutdated;
		LatteTC_ResetTextureChangeTracker(tex, true);
	}
	LatteTextureLoader_loadTextureDataIntoSlice(tex, ctx.width, ctx.height, depth, mipLevels, pixelData, sliceIndex, mipIndex, imageSize);

	if (ctx.dump)
	{
		fs::path path = ActiveSettings::GetUserDataPath("dump/textures");
		path /= fmt::format("{:08x}_fmt{:04x}_slice{:d}_mip{:02d}_{:d}x{:d}_tm{:02d}.tga", physImagePtr, (uint32)tex->format, sliceIndex, mipIndex, tex->width, tex->height, tileMode);
		tga_write_rgba(path, ctx.width, ctx.height, ctx.dumpRGBA);
		free(ctx.dumpRGBA);
	}
	g_renderer->texture_releaseTextureUploadBuffer(pixelData);
	catchOpenGLError();
}

// --- readback (host -> guest memory) ---------------------------------------

// copies each row of a linear guest surface from a host buffer, as whole elements
static void LatteTextureLoader_writeLinearRows(LatteTextureLoaderCtx* ctx, const uint8* source, uint32 sourceRowPitch, uint32 elementSize)
{
	const size_t sliceElements = (size_t)ctx->sliceIndex * ctx->surfaceInfoHeight * ctx->pitch;
	for (sint32 y = 0; y < ctx->height; y++)
	{
		uint8* dst = ctx->inputData + ((size_t)y * ctx->pitch + sliceElements) * elementSize;
		memcpy(dst, source + (size_t)y * sourceRowPitch, (size_t)ctx->width * elementSize);
	}
}

// copies elements one by one through the linear address function
static void LatteTextureLoader_writeLinearElements(LatteTextureLoaderCtx* ctx, const uint8* source, uint32 sourceRowPitch, uint32 elementSize)
{
	for (sint32 y = 0; y < ctx->height; y += ctx->stepY)
	{
		const uint8* src = source + (size_t)y * sourceRowPitch;
		for (sint32 x = 0; x < ctx->width; x += ctx->stepX, src += elementSize)
			memcpy(LatteTextureLoader_getInputLinearOptimized(ctx, x, y), src, elementSize);
	}
}

void LatteTextureLoader_writeReadbackTextureToMemory(LatteTextureDefinition* textureData, uint32 sliceIndex, uint32 mipIndex, uint8* linearPixelData, uint32 sourceRowPitch)
{
	using Latte::E_GX2SURFFMT;
	LatteTextureLoaderCtx ctx = {};
	LatteTextureLoader_begin(&ctx, sliceIndex, mipIndex, textureData->physAddress, textureData->physMipAddress, textureData->format, textureData->dim, textureData->width, textureData->height, textureData->depth, textureData->mipLevels, textureData->pitch, textureData->tileMode, textureData->swizzle);
	if (ctx.physAddress == MPTR_NULL)
	{
		cemuLog_log(LogType::Force, "Texture readback: texture has no address");
		return;
	}
	cemuLog_log(LogType::TextureReadback, "[TextureReadback-Write] PhysAddr {:08x} Res {}x{} Fmt {} Slice {} Mip {}", textureData->physAddress, textureData->width, textureData->height, textureData->format, sliceIndex, mipIndex);

	const E_GX2SURFFMT format = textureData->format;
	if (textureData->tileMode == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
	{
		switch (format)
		{
		case E_GX2SURFFMT::R8_G8_B8_A8_UNORM:
		case E_GX2SURFFMT::R8_G8_B8_A8_SRGB:
			LatteTextureLoader_writeLinearRows(&ctx, linearPixelData, sourceRowPitch, 4);
			break;
		case E_GX2SURFFMT::R16_G16_B16_A16_UNORM:
			LatteTextureLoader_writeLinearRows(&ctx, linearPixelData, sourceRowPitch, 8);
			break;
		case E_GX2SURFFMT::R8_G8_UNORM:
		case E_GX2SURFFMT::R16_UNORM:
			LatteTextureLoader_writeLinearRows(&ctx, linearPixelData, sourceRowPitch, 2);
			break;
		case E_GX2SURFFMT::R32_G32_B32_A32_FLOAT:
		case E_GX2SURFFMT::R32_G32_B32_A32_SINT:
		case E_GX2SURFFMT::R32_G32_B32_A32_UINT:
			LatteTextureLoader_writeLinearElements(&ctx, linearPixelData, sourceRowPitch, 16);
			break;
		case E_GX2SURFFMT::R16_G16_B16_A16_FLOAT:
			LatteTextureLoader_writeLinearElements(&ctx, linearPixelData, sourceRowPitch, 8);
			break;
		case E_GX2SURFFMT::R32_FLOAT:
			LatteTextureLoader_writeLinearElements(&ctx, linearPixelData, sourceRowPitch, 4);
			break;
		default:
			cemuLog_logDebug(LogType::Force, "Texture readback: linear format 0x{:04x} not supported", (uint32)format);
			debugBreakpoint();
			break;
		}
		return;
	}

	// tiled surfaces go pixel by pixel through the address functions
	const Latte::E_HWFMT hwFormat = Latte::GetHWFormat(format);
	if (hwFormat == Latte::E_HWFMT::HWFMT_8_8_8_8 || hwFormat == Latte::E_HWFMT::HWFMT_32_FLOAT)
	{
		// 8_8_8_8: Bayonetta 2; 32_FLOAT: Wind Waker reads its depth buffer this way
		for (sint32 y = 0; y < ctx.height; y++)
		{
			const uint8* src = linearPixelData + (size_t)y * sourceRowPitch;
			for (sint32 x = 0; x < ctx.width; x++, src += 4)
				memcpy(LatteTextureLoader_GetInput(&ctx, x, y), src, 4);
		}
	}
	else
	{
		cemuLog_logDebug(LogType::Force, "Texture readback: format 0x{:04x} with tile mode 0x{:02x} not supported", (uint32)format, textureData->tileMode);
	}
}

// Guest address range a texture level touches, estimated from its four corner pixels
void LatteTextureLoader_estimateAccessedDataRange(LatteTexture* texture, sint32 sliceIndex, sint32 mipIndex, uint32& addrStart, uint32& addrEnd)
{
	LatteTextureLoaderCtx ctx = {};
	LatteTextureLoader_begin(&ctx, sliceIndex, mipIndex, texture->physAddress, texture->physMipAddress, texture->format, texture->dim, texture->width, texture->height, texture->depth, texture->mipLevels, texture->pitch, texture->tileMode, texture->swizzle);
	cemu_assert_debug(ctx.width > 0 && ctx.height > 0);

	const sint32 corners[4][2] = { {0, 0}, {ctx.width - 1, 0}, {0, ctx.height - 1}, {ctx.width - 1, ctx.height - 1} };
	uint32 lo = 0xFFFFFFFF;
	uint32 hi = 0;
	for (const auto& c : corners)
	{
		const uint32 addr = memory_getVirtualOffsetFromPointer(LatteTextureLoader_GetInput(&ctx, c[0], c[1]));
		lo = std::min(lo, addr);
		hi = std::max(hi, addr);
	}
	addrStart = lo;
	addrEnd = hi;
}
