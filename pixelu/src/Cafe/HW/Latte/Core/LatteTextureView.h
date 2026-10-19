// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// A range of mips and slices of a texture, possibly reinterpreted with another format
// or dimension. Backends derive from this class for their host view objects.
class LatteTextureView
{
public:
	enum class MagFilter
	{
		kLinear,
		kNearestNeighbor,
	};

	// registerView: add the view to its texture and make it findable through LatteTextureViewLookupCache
	LatteTextureView(class LatteTexture* texture, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, bool registerView = true);
	virtual ~LatteTextureView();

	// makes the view findable through the guest address of one of the texture's subresources
	void CreateLookupForSubTexture(uint32 mipStart, uint32 sliceStart);

	class LatteTexture* baseTexture;
	// Range within baseTexture. The same view may be found through several guest
	// addresses (see CreateLookupForSubTexture), so these values are not necessarily the
	// mip/slice the guest registers asked for.
	sint32 firstMip;
	sint32 numMip;
	sint32 firstSlice;
	sint32 numSlice;
	Latte::E_GX2SURFFMT format; // may differ from the texture format
	Latte::E_DIM dim;
	uint32 lastTextureBindIndex = 0;
	// framebuffer objects
	std::vector<class LatteCachedFBO*> list_fboLookup; // FBOs found through this view (first color buffer, or the depth buffer of depth-only FBOs)
	std::vector<class LatteCachedFBO*> list_associatedFbo; // every FBO that uses this view
	// guest addresses under which the view is registered in the lookup cache
	std::vector<MPTR> lookupAddresses;
};

// Finds views by the guest surface parameters they were registered with
class LatteTextureViewLookupCache
{
public:
	static void Add(LatteTextureView* view, uint32 baseMip = 0, uint32 baseSlice = 0);
	static void RemoveAll(LatteTextureView* view);

	static LatteTextureView* lookup(MPTR physAddr, sint32 width, sint32 height, sint32 depth, sint32 pitch, sint32 firstMip, sint32 numMip, sint32 firstSlice, sint32 numSlice, Latte::E_GX2SURFFMT format, Latte::E_DIM dim);
	static LatteTextureView* lookupWithColorOrDepthType(MPTR physAddr, sint32 width, sint32 height, sint32 depth, sint32 pitch, sint32 firstMip, sint32 numMip, sint32 firstSlice, sint32 numSlice, Latte::E_GX2SURFFMT format, Latte::E_DIM dim, bool isDepth);
	// mip and slice count are not compared
	static LatteTextureView* lookupSlice(MPTR physAddr, sint32 width, sint32 height, sint32 pitch, sint32 firstMip, sint32 firstSlice, Latte::E_GX2SURFFMT format);
	// like lookupSlice, but any view at least minWidth x minHeight large matches
	static LatteTextureView* lookupSliceMinSize(MPTR physAddr, sint32 minWidth, sint32 minHeight, sint32 pitch, sint32 firstMip, sint32 firstSlice, Latte::E_GX2SURFFMT format);
	// like lookupSlice, but the depth flag has to match as well
	static LatteTextureView* lookupSliceEx(MPTR physAddr, sint32 width, sint32 height, sint32 pitch, sint32 firstMip, sint32 firstSlice, Latte::E_GX2SURFFMT format, bool isDepth);

	static std::unordered_set<LatteTextureView*> GetAllViews();
};
