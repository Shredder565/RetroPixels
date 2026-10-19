// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteConst.h"

// Sampler settings a host backend derives from the Latte sampler registers
struct LatteSamplerState
{
	uint8 clampS;
	uint8 clampT;
	uint8 clampR;
	uint32 filterMag; // OpenGL enum value
	uint32 filterMin; // OpenGL enum value
	uint8 maxAniso;
	uint8 maxMipLevels;
	uint8 depthCompareMode;
	uint8 depthCompareFunc;
	uint16 minLod;
	uint16 maxLod;
	sint16 lodBias;
	uint8 borderType;
	float borderColor[4];
};

#include "Cafe/HW/Latte/Core/LatteTextureView.h"

// A guest surface mirrored on the host. Backends derive from this class and own the
// actual GPU resource; the texture cache tracks the guest-side layout and how the
// surface shares memory with other cached textures.
class LatteTexture
{
public:
	LatteTexture(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth);
	virtual ~LatteTexture();

	virtual void AllocateOnHost() = 0;

	// returns an existing view with exactly these parameters or creates one
	LatteTextureView* GetOrCreateView(Latte::E_DIM viewDim, Latte::E_GX2SURFFMT viewFormat, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
	{
		for (LatteTextureView* view : views)
		{
			const bool sameRange = view->firstMip == firstMip && view->numMip == mipCount && view->firstSlice == firstSlice && view->numSlice == sliceCount;
			if (sameRange && view->dim == viewDim && view->format == viewFormat)
				return view;
		}
		return CreateView(viewDim, viewFormat, firstMip, mipCount, firstSlice, sliceCount);
	}

	LatteTextureView* GetOrCreateView(sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount)
	{
		return GetOrCreateView(dim, format, firstMip, mipCount, firstSlice, sliceCount);
	}

	bool IsCompressedFormat() const { return Latte::IsCompressedFormat(format); }
	uint32 GetBPP() const { return Latte::GetFormatBits(format); }
	bool Is3DTexture() const { return dim == Latte::E_DIM::DIM_3D; }

	// guest size of a mip level
	void GetSize(sint32& outWidth, sint32& outHeight, sint32 mipLevel) const
	{
		outWidth = std::max(1, width >> mipLevel);
		outHeight = std::max(1, height >> mipLevel);
	}

	// host size of a mip level, which differs from the guest size when a graphic pack rule resized the texture
	void GetEffectiveSize(sint32& outWidth, sint32& outHeight, sint32 mipLevel) const
	{
		const sint32 baseWidth = overwriteInfo.hasResolutionOverwrite ? overwriteInfo.width : width;
		const sint32 baseHeight = overwriteInfo.hasResolutionOverwrite ? overwriteInfo.height : height;
		outWidth = std::max(1, baseWidth >> mipLevel);
		outHeight = std::max(1, baseHeight >> mipLevel);
	}

	// number of slices in a mip level (only 3D textures shrink in depth)
	sint32 GetMipDepth(sint32 mipIndex)
	{
		cemu_assert_debug(mipIndex >= 0 && mipIndex < mipLevels);
		return Is3DTexture() ? std::max(depth >> mipIndex, 1) : depth;
	}

	sint32 GetMipWidth(sint32 mipIndex)
	{
		cemu_assert_debug(mipIndex >= 0 && mipIndex < mipLevels);
		return std::max(width >> mipIndex, 1);
	}

	sint32 GetMipHeight(sint32 mipIndex)
	{
		cemu_assert_debug(mipIndex >= 0 && mipIndex < mipLevels);
		return std::max(height >> mipIndex, 1);
	}

	// Per-subresource data lives in a flat array with depth*mipLevels entries. Entry
	// (slice, mip) sits at depth*mip+slice; for 3D textures the slots of slices that a
	// smaller mip no longer has are simply unused.
	sint32 GetSliceMipArraySize()
	{
		cemu_assert_debug(depth > 0 && mipLevels > 0);
		return depth * mipLevels;
	}

	sint32 GetSliceMipArrayIndex(sint32 sliceIndex, sint32 mipIndex)
	{
		cemu_assert_debug(sliceIndex < depth && mipIndex < mipLevels);
		return depth * mipIndex + sliceIndex;
	}

	struct LatteTextureSliceMipInfo* GetSliceMipArrayEntry(sint32 sliceIndex, sint32 mipIndex);
	// every live texture; deleted textures leave nullptr holes that are reused later
	static std::vector<LatteTexture*>& GetAllTextures();

protected:
	virtual LatteTextureView* CreateView(Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount) = 0;

public:
	// guest surface description
	MPTR physAddress;
	MPTR physMipAddress;
	Latte::E_DIM dim;
	Latte::E_GX2SURFFMT format;
	Latte::E_HWTILEMODE tileMode;
	sint32 width;
	sint32 height;
	sint32 depth;
	sint32 pitch;
	sint32 mipLevels; // mip levels in use, at least 1
	sint32 maxPossibleMipLevels{}; // mip levels the (possibly resized) host texture could have
	uint32 swizzle;
	uint32 lastRenderTargetSwizzle{}; // swizzle used the last time the texture was a render target
	// content
	bool isDataDefined{};
	bool isDepth;
	bool hasStencil{};
	struct LatteTextureSliceMipInfo* sliceMipInfo{}; // one entry per subresource, see GetSliceMipArrayIndex
	// guest memory range whose hash detects CPU-side modifications
	MPTR texDataPtrLow{};
	MPTR texDataPtrHigh{};
	uint32 texDataHash2{};
	// GPU-side state
	bool isUpdatedOnGPU{ false }; // a GPU write made the host copy newer than guest memory
	bool enableReadback{ false }; // the texture may be copied back to guest memory
	bool forceInvalidate{}; // next change check treats the data as modified
	bool reloadFromDynamicTextures{}; // a texture sharing memory was written on the GPU
	uint64 lastWriteEventCounter;
	uint64 lastUpdateEventCounter;
	uint32 lastUpdateFrameCounter{};
	uint32 reloadCount{};
	uint32 lastDataUpdateFrameCounter{}; // frame of the last guest memory change check
	bool useLightHash{}; // sample fewer words when hashing (textures written on the GPU)
	// values from graphic pack texture rules
	struct
	{
		bool hasResolutionOverwrite;
		sint32 width;
		sint32 height;
		sint32 depth;
		bool hasFormatOverwrite;
		sint32 format;
		sint16 lodBias; // 1/64 steps
		bool hasLodBias;
		sint16 relativeLodBias; // 1/64 steps
		bool hasRelativeLodBias;
		sint8 anisotropicLevel{ -1 }; // log2 of the anisotropy, 0 disables it
	}overwriteInfo{};
	// detection of a texture being bound more than once per draw
	uint32 lockTextureUpdateId{};
	uint32 lockCount{};
	// garbage collection
	uint32 lastAccessTick{};
	uint32 lastAccessFrameCount{};
	// draw index of the last unflushed render target write (render feedback loops)
	uint32 lastUnflushedRTDrawcallIndex{};
	// views
	LatteTextureView* baseView{};
	std::vector<LatteTextureView*> views; // all registered views including baseView
	// textures that share memory with this one in a data compatible way
	std::vector<struct LatteTextureRelation*> list_compatibleRelations;
	// slot in GetAllTextures()
	size_t globalListIndex;
};

// plain copy of a texture's guest description
struct LatteTextureDefinition
{
	MPTR physAddress;
	MPTR physMipAddress;
	Latte::E_DIM dim;
	Latte::E_GX2SURFFMT format;
	uint32 width;
	uint32 height;
	uint32 depth;
	uint32 pitch;
	uint32 mipLevels;
	uint32 swizzle;
	Latte::E_HWTILEMODE tileMode;
	bool isDepth;

	LatteTextureDefinition() = default;
	LatteTextureDefinition(const LatteTexture* tex)
		: physAddress(tex->physAddress), physMipAddress(tex->physMipAddress), dim(tex->dim), format(tex->format),
		width(tex->width), height(tex->height), depth(tex->depth), pitch(tex->pitch), mipLevels(tex->mipLevels),
		swizzle(tex->swizzle), tileMode(tex->tileMode), isDepth(tex->isDepth)
	{}
};

class LatteTextureView;

// a subresource of another texture whose memory overlaps a subresource of this one
struct LatteTextureSliceMipDataOverlap_t
{
	LatteTexture* destTexture;
	struct LatteTextureSliceMipInfo* destMipSliceInfo;
};

// guest layout and change tracking of one slice of one mip level
struct LatteTextureSliceMipInfo
{
	uint32 addrStart; // where the subresource starts in guest memory
	uint32 addrEnd;
	uint32 subIndex; // slice within the data range for thick tile modes (several slices share one range)
	LatteTexture* texture;
	sint32 sliceIndex;
	sint32 mipIndex;
	uint32 dataChecksum;
	uint64 lastDynamicUpdate; // update event of the last GPU write
	Latte::E_HWTILEMODE tileMode;
	sint32 pitch;
	// part of [addrStart, addrEnd) the texture data actually touches
	uint32 estDataAddrStart;
	uint32 estDataAddrEnd;
	std::vector<LatteTextureSliceMipDataOverlap_t> list_dataOverlap;
};

// subTexture is stored inside baseTexture starting at the given slice/mip
struct LatteTextureRelation
{
	LatteTexture* baseTexture;
	LatteTexture* subTexture;
	sint32 baseSliceIndex;
	sint32 baseMipIndex;
	sint32 sliceCount;
	sint32 mipCount;
	sint32 yOffset;
};

// texture cache snapshot for debugging tools
struct LatteTextureViewInformation
{
	MPTR physAddress;
	MPTR physMipAddress;
	sint32 width;
	sint32 height;
	sint32 pitch;
	sint32 firstMip;
	sint32 numMip;
	sint32 firstSlice;
	sint32 numSlice;
	Latte::E_GX2SURFFMT format;
	Latte::E_DIM dim;
};

struct LatteTextureInformation
{
	MPTR physAddress;
	MPTR physMipAddress;
	sint32 width;
	sint32 height;
	sint32 depth;
	sint32 pitch;
	sint32 mipLevels;
	Latte::E_GX2SURFFMT format;
	bool isDepth;
	Latte::E_DIM dim;
	Latte::E_HWTILEMODE tileMode;
	uint32 lastAccessTick;
	uint32 lastAccessFrameCount;
	bool isUpdatedOnGPU;
	uint32 alternativeViewCount;
	struct
	{
		bool hasResolutionOverwrite;
		sint32 width;
		sint32 height;
		sint32 depth;
	}overwriteInfo{};

	std::vector<LatteTextureViewInformation> views;
};

// Records whether a texture gets deleted while the guard exists, for code that keeps a
// texture pointer across calls that may delete textures.
class LatteTextureDeleteGuard
{
public:
	explicit LatteTextureDeleteGuard(LatteTexture* texture);
	~LatteTextureDeleteGuard();
	LatteTextureDeleteGuard(const LatteTextureDeleteGuard&) = delete;
	LatteTextureDeleteGuard& operator=(const LatteTextureDeleteGuard&) = delete;

	bool WasDeleted() const { return m_deleted; }

	static void NotifyDeletion(LatteTexture* texture);

private:
	LatteTexture* m_texture;
	bool m_deleted{ false };
	LatteTextureDeleteGuard* m_outer;
};

void LatteTexture_init();
void LatteTexture_updateTextures();

std::vector<LatteTextureInformation> LatteTexture_QueryCacheInfo();

float* LatteTexture_getEffectiveTextureScale(LatteConst::ShaderType shaderType, sint32 texUnit);

LatteTextureView* LatteTexture_CreateTexture(Latte::E_DIM dim, MPTR physAddress, MPTR physMipAddress, Latte::E_GX2SURFFMT format, uint32 width, uint32 height, uint32 depth, uint32 pitch, uint32 mipLevels, uint32 swizzle, Latte::E_HWTILEMODE tileMode, bool isDepth);
void LatteTexture_Delete(LatteTexture* texture);

void LatteTextureLoader_writeReadbackTextureToMemory(LatteTextureDefinition* textureData, uint32 sliceIndex, uint32 mipIndex, uint8* linearPixelData, uint32 sourceRowPitch);

sint32 LatteTexture_getEffectiveWidth(LatteTexture* texture);
bool LatteTexture_doesEffectiveRescaleRatioMatch(LatteTexture* texture1, sint32 mipLevel1, LatteTexture* texture2, sint32 mipLevel2);
void LatteTexture_scaleToEffectiveSize(LatteTexture* texture, sint32* x, sint32* y, sint32 mipLevel);
uint64 LatteTexture_getNextUpdateEventCounter();

void LatteTexture_UpdateCacheFromDynamicTextures(LatteTexture* texture);
void LatteTexture_MarkConnectedTexturesForReloadFromDynamicTextures(LatteTexture* texture);
void LatteTexture_TrackTextureGPUWrite(LatteTexture* texture, uint32 slice, uint32 mip, uint64 eventCounter);

void LatteTexture_InitSliceAndMipInfo(LatteTexture* texture);
void LatteTexture_RegisterTextureMemoryOccupancy(LatteTexture* texture);
void LatteTexture_UnregisterTextureMemoryOccupancy(LatteTexture* texture);

void LatteTexture_Invalidate(uint32 physAddr, uint32 size);

void LatteTexture_DeleteTextureRelations(LatteTexture* texture);
void LatteTexture_DeleteDataOverlapTracking(LatteTexture* texture);

LatteTextureView* LatteTexture_CreateMapping(MPTR physAddr, MPTR physMipAddr, sint32 width, sint32 height, sint32 depth, sint32 pitch, Latte::E_HWTILEMODE tileMode, uint32 swizzle, sint32 firstMip, sint32 numMip, sint32 firstSlice, sint32 numSlice, Latte::E_GX2SURFFMT format, Latte::E_DIM dimBase, Latte::E_DIM dimView, bool isDepth, bool allowCreateNewDataTexture = true);

LatteTextureView* LatteTC_LookupTextureByData(MPTR physAddr, sint32 width, sint32 height, sint32 pitch, sint32 firstMip, sint32 numMip, sint32 firstSlice, sint32 numSlice, sint32* searchIndex);
void LatteTC_LookupTexturesByPhysAddr(MPTR physAddr, std::vector<LatteTexture*>& list_textures);

LatteTextureView* LatteTC_GetTextureSliceViewOrTryCreate(MPTR srcImagePtr, MPTR srcMipPtr, Latte::E_GX2SURFFMT srcFormat, Latte::E_HWTILEMODE srcTileMode, uint32 srcWidth, uint32 srcHeight, uint32 srcDepth, uint32 srcPitch, uint32 srcSwizzle, uint32 srcSlice, uint32 srcMip, const bool requireExactResolution = false);

void LatteTexture_MarkDynamicTextureAsChanged(LatteTextureView* textureView, sint32 sliceIndex, sint32 mipIndex, uint64 eventCounter);
void LatteTexture_UpdateTextureFromDynamicChanges(LatteTexture* texture);

void LatteTexture_UpdateDataToLatest(LatteTexture* texture);
