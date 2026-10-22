// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Common/cpu_features.h"

// Texture cache bookkeeping: change detection for guest memory, garbage collection
// and deletion of cached textures.

static sint32 s_registeredTextureCount = 0;

void LatteTC_Init()
{
	cemu_assert_debug(s_registeredTextureCount == 0);
}

void LatteTC_RegisterTexture(LatteTexture* tex)
{
	s_registeredTextureCount++;
}

void LatteTC_UnregisterTexture(LatteTexture* tex)
{
	s_registeredTextureCount--;
}

// --- change detection ------------------------------------------------------------
// Instead of hashing whole textures a fixed pattern of words is sampled. The pattern
// depends on the texture size and format; changing it would make textures reload
// at different times, so it is kept stable.

namespace
{
	inline uint32 Rotl32(uint32 v, uint32 n) { return (v << n) | (v >> (32 - n)); }
	inline uint64 Rotl64(uint64 v, uint32 n) { return (v << n) | (v >> (64 - n)); }

	// rotate-and-add over `count` words, `stride` words apart
	uint32 SampleHash32(const uint32* data, uint32 count, uint32 stride)
	{
		uint32 h = 0;
		for (uint32 i = 0; i < count; i++, data += stride)
			h = Rotl32(h + *data, 3);
		return h;
	}

	// 37 evenly spread 64-bit samples (a prime count avoids lining up with the row pitch)
	uint32 SampleHashSparse(const void* texData, uint32 byteSize, bool sample16Bytes)
	{
		const uint64* p = (const uint64*)texData;
		const uint32 step = (byteSize / 8) / 37;
		uint64 h = 0;
		for (sint32 i = 0; i < 37; i++, p += step)
			h = Rotl64(h + (sample16Bytes ? (p[0] ^ p[1]) : p[0]), 3);
		return (uint32)h ^ (uint32)(h >> 32);
	}

	// every 32nd 64-bit word
	uint32 SampleHashLarge(const void* texData, uint32 byteSize)
	{
		const uint64* p = (const uint64*)texData;
		uint64 h = 0;
		for (uint32 n = byteSize / 256; n > 0; n--, p += 32)
			h = Rotl64(h + *p, 3);
		return (uint32)(h & 0xFFFFFFFF) + (uint32)(h >> 32);
	}

	bool IsBC23Format(Latte::E_GX2SURFFMT format)
	{
		using F = Latte::E_GX2SURFFMT;
		return format == F::BC2_UNORM || format == F::BC2_SRGB || format == F::BC3_UNORM || format == F::BC3_SRGB;
	}
}

uint32 LatteTexture_CalculateTextureDataHash(LatteTexture* hostTexture)
{
	if (hostTexture->texDataPtrHigh == hostTexture->texDataPtrLow)
		return 0;
	const uint32* data = (const uint32*)memory_getPointerFromPhysicalOffset(hostTexture->texDataPtrLow);
	if (hostTexture->format == Latte::E_GX2SURFFMT::R11_G11_B10_FLOAT)
	{
		// This format is practically never written by the CPU, so only the first words
		// are checked. A full upload still changes them, and BotW writes garbage into the
		// unused parts of such textures which would otherwise cause constant reloads.
		const bool isW101Volume = hostTexture->tileMode == Latte::E_HWTILEMODE::TM_1D_TILED_THICK && hostTexture->width == 8 && hostTexture->height == 8 && hostTexture->depth == 8;
		if (isW101Volume)
		{
			// Wonderful 101 fills an 8x8x8 volume through the tiling aperture; sample the
			// four layers of the first thick slice (0x100 bytes apart)
			return data[0] ^ data[0x40] ^ data[0x80] ^ data[0xC0];
		}
		return data[0] ^ data[1] ^ data[2] ^ data[3];
	}

	const uint32 byteSize = hostTexture->texDataPtrHigh - hostTexture->texDataPtrLow;
	if (hostTexture->IsCompressedFormat() || hostTexture->useLightHash)
	{
		if (byteSize < 256)
			return SampleHash32(data, byteSize / 4, 1);
		return SampleHashSparse(data, byteSize, IsBC23Format(hostTexture->format));
	}

	const uint32 pixelCount = hostTexture->width * hostTexture->height;
	if (pixelCount <= 700 * 700)
		return SampleHash32(data, byteSize / 16, 4);
	if (pixelCount <= 1200 * 1200)
		return SampleHash32(data, byteSize / 48, 12);
#if BOOST_OS_WINDOWS
	if (g_CPUFeatures.x86.avx2)
	{
		// xor of every ninth 32-byte block, folded to 32 bits
		__m256i acc = _mm256_setzero_si256();
		const __m256i* p = (const __m256i*)data;
		for (uint32 n = byteSize / 288; n > 0; n--, p += 9)
			acc = _mm256_xor_si256(acc, _mm256_loadu_si256(p));
		alignas(32) uint32 lanes[8];
		_mm256_store_si256((__m256i*)lanes, acc);
		uint32 h = 0;
		for (uint32 lane : lanes)
			h += lane;
		return h;
	}
#endif
	return SampleHashLarge(data, byteSize);
}

static uint64 s_botwTerrainArrayChangeFrame = 0;

bool LatteTC_HasTextureChanged(LatteTexture* hostTexture, bool force)
{
	if (hostTexture->forceInvalidate)
	{
		force = true;
		hostTexture->forceInvalidate = false;
	}
	// guest memory is checked at most once per frame
	if (hostTexture->lastDataUpdateFrameCounter == LatteGPUState.frameCounter && !force)
		return false;
	hostTexture->lastDataUpdateFrameCounter = LatteGPUState.frameCounter;
	// 1280-wide textures are assumed to be render targets the CPU never writes (except these two formats)
	if (!force && hostTexture->width == 1280 && hostTexture->format != Latte::E_GX2SURFFMT::R8_UNORM && hostTexture->format != Latte::E_GX2SURFFMT::BC3_UNORM)
		return false;
	// BotW's 1024x1024x83 terrain array can be corrupted after video playback. When it
	// changes, it is reloaded once more in a later frame as well.
	const bool isBotwTerrainArray = hostTexture->depth == 83 && hostTexture->width == 1024 && hostTexture->height == 1024;
	const uint32 hash = LatteTexture_CalculateTextureDataHash(hostTexture);
	if (hash != hostTexture->texDataHash2)
	{
		hostTexture->texDataHash2 = hash;
		if (isBotwTerrainArray)
			s_botwTerrainArrayChangeFrame = LatteGPUState.frameCounter;
		return true;
	}
	if (s_botwTerrainArrayChangeFrame != 0 && isBotwTerrainArray && s_botwTerrainArrayChangeFrame != LatteGPUState.frameCounter)
	{
		s_botwTerrainArrayChangeFrame = 0;
		return true;
	}
	return false;
}

// Textures written on the GPU switch to the lighter sampling pattern. The stored hash
// is recomputed, so CPU writes since the last check are not detected.
static void LatteTC_SwitchToLightHash(LatteTexture* hostTexture)
{
	if (hostTexture->useLightHash)
		return;
	hostTexture->useLightHash = true;
	hostTexture->texDataHash2 = LatteTexture_CalculateTextureDataHash(hostTexture);
}

void LatteTC_FlagSliceAsGPUUpdated(LatteTexture* hostTexture, uint32 sliceIndex, uint32 mipIndex)
{
	// tracked for the whole texture, not per subresource
	hostTexture->isUpdatedOnGPU = true;
	LatteTC_SwitchToLightHash(hostTexture);
}

void LatteTC_ResetTextureChangeTracker(LatteTexture* hostTexture, bool force)
{
	if (hostTexture->lastDataUpdateFrameCounter == LatteGPUState.frameCounter && !force)
		return;
	hostTexture->lastDataUpdateFrameCounter = LatteGPUState.frameCounter;
	LatteTC_HasTextureChanged(hostTexture, true);
}

void LatteTC_MarkTextureStillInUse(LatteTexture* texture)
{
	texture->lastAccessTick = LatteGPUState.currentDrawCallTick;
	texture->lastAccessFrameCount = LatteGPUState.frameCounter;
}

// true if each of the first three mip levels was written over by newer GPU data of
// overlapping textures, meaning the texture's own contents are dead
static bool LatteTC_IsTextureDataOverwritten(LatteTexture* texture)
{
	const sint32 mipsToCheck = std::min(texture->mipLevels, 3);
	for (sint32 mip = 0; mip < mipsToCheck; mip++)
	{
		const sint32 slices = texture->Is3DTexture() ? std::max(1, texture->depth >> mip) : texture->depth;
		for (sint32 slice = 0; slice < slices; slice++)
		{
			const LatteTextureSliceMipInfo* info = texture->GetSliceMipArrayEntry(slice, mip);
			const bool outdated = std::any_of(info->list_dataOverlap.begin(), info->list_dataOverlap.end(),
				[info](const LatteTextureSliceMipDataOverlap_t& o) { return info->lastDynamicUpdate < o.destMipSliceInfo->lastDynamicUpdate; });
			if (!outdated)
				return false;
		}
	}
	return true;
}

void LatteTexture_Delete(LatteTexture* texture)
{
	LatteTextureDeleteGuard::NotifyDeletion(texture);
	LatteTC_UnregisterTexture(texture);
	LatteMRT::NotifyTextureDeletion(texture);
	LatteTextureReadback_NotifyTextureDeletion(texture);
	LatteTexture_DeleteTextureRelations(texture);
	while (!texture->views.empty())
		delete texture->views.front();
	cemu_assert_debug(texture->baseView == nullptr);
	LatteTexture_DeleteDataOverlapTracking(texture);
	LatteTexture_UnregisterTextureMemoryOccupancy(texture);
	delete[] texture->sliceMipInfo;
	texture->sliceMipInfo = nullptr;
	delete texture;
}

// --- garbage collection ----------------------------------------------------------

// deletes the texture if it is no longer needed, returns true if it was deleted
static bool LatteTC_CleanupCheckTexture(LatteTexture* texture, uint32 currentTick)
{
	const uint32 ticksUnused = currentTick - texture->lastAccessTick;
	const uint32 framesUnused = LatteGPUState.frameCounter - texture->lastAccessFrameCount;
	// textures that only mirror guest memory can be recreated from it at any time
	if (!texture->isUpdatedOnGPU && ticksUnused >= 120 * 1000 && framesUnused >= 2000)
	{
		LatteTexture_Delete(texture);
		return true;
	}
	// contents completely replaced through other textures
	if ((LatteGPUState.currentDrawCallTick - texture->lastAccessTick) >= 100 && LatteTC_IsTextureDataOverwritten(texture))
	{
		LatteTexture_Delete(texture);
		return true;
	}
	// views are cheap to recreate, drop up to three extra ones after five unused seconds
	if (ticksUnused >= 5 * 1000 && framesUnused >= 30)
	{
		for (sint32 n = 0; n < 3 && texture->views.size() > 1; n++)
		{
			LatteTextureView* victim = texture->views[0] == texture->baseView ? texture->views[1] : texture->views[0];
			delete victim;
		}
	}
	return false;
}

void LatteTexture_RefreshInfoCache();

// called once per frame; checks a few textures each time, round robin
void LatteTC_CleanupUnusedTextures()
{
	static size_t s_scanPosition = 0;
	constexpr sint32 kChecksPerCall = 25;
	constexpr sint32 kMaxDeletesPerCall = 10; // deleting is expensive and could stutter
	const uint32 currentTick = GetTickCount();
	std::vector<LatteTexture*>& textures = LatteTexture::GetAllTextures();
	if (!textures.empty())
	{
		sint32 deletesLeft = kMaxDeletesPerCall;
		for (sint32 n = 0; n < kChecksPerCall; n++)
		{
			if (s_scanPosition >= textures.size())
				s_scanPosition = 0;
			LatteTexture* tex = textures[s_scanPosition++];
			if (!tex || !LatteTC_CleanupCheckTexture(tex, currentTick))
				continue;
			if (--deletesLeft <= 0 || textures.empty())
				break;
		}
	}
	LatteTexture_RefreshInfoCache();
}

// textures that may be deleted when host memory runs out
std::vector<LatteTexture*> LatteTC_GetDeleteableTextures()
{
	std::vector<LatteTexture*> result;
	const uint32 frame = LatteGPUState.frameCounter;
	for (LatteTexture* tex : LatteTexture::GetAllTextures())
	{
		if (!tex || tex->lastAccessFrameCount == 0)
			continue; // empty slot or texture still being set up
		if (frame - tex->lastAccessFrameCount < 3)
			continue;
		if (!tex->isUpdatedOnGPU || LatteTC_IsTextureDataOverwritten(tex))
			result.emplace_back(tex);
	}
	return result;
}

void LatteTC_UnloadAllTextures()
{
	const std::vector<LatteTexture*> textures = LatteTexture::GetAllTextures();
	for (LatteTexture* tex : textures)
	{
		if (tex)
			LatteTexture_Delete(tex);
	}
	LatteRenderTarget_unloadAll();
}
