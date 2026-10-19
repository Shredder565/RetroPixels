// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteTextureView.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"

LatteTextureView::LatteTextureView(LatteTexture* texture, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, bool registerView)
	: baseTexture(texture), firstMip(firstMip), numMip(mipCount), firstSlice(firstSlice), numSlice(sliceCount), format(format), dim(dim)
{
	if (!registerView)
		return;
	texture->views.emplace_back(this);
	LatteTextureViewLookupCache::Add(this);
}

LatteTextureView::~LatteTextureView()
{
	LatteTextureViewLookupCache::RemoveAll(this);
	vectorRemoveByValue(baseTexture->views, this);
	if (baseTexture->baseView == this)
		baseTexture->baseView = nullptr;
	// FBOs that reference the view can no longer be used
	while (!list_associatedFbo.empty())
		LatteMRT::DeleteCachedFBO(list_associatedFbo.front());
}

void LatteTextureView::CreateLookupForSubTexture(uint32 mipStart, uint32 sliceStart)
{
	// the lookup through the texture's own address is created by the constructor
	cemu_assert_debug(mipStart != 0 || sliceStart != 0);
	LatteTextureViewLookupCache::Add(this, mipStart, sliceStart);
}

// --- lookup cache ---------------------------------------------------------------
// Every registration is stored under its guest address. All lookups compare the
// address exactly, so a list per address holds every candidate; within a list
// registrations keep their insertion order and the oldest match wins.

namespace
{
	struct ViewRegistration
	{
		sint32 width;
		sint32 height;
		sint32 pitch;
		sint32 firstMip;
		sint32 numMip;
		sint32 firstSlice;
		sint32 numSlice;
		Latte::E_GX2SURFFMT format;
		Latte::E_DIM dim;
		bool isDepth;
		LatteTextureView* view;
	};

	std::unordered_map<MPTR, std::vector<ViewRegistration>>& RegistrationsByAddress()
	{
		static std::unordered_map<MPTR, std::vector<ViewRegistration>> s_map;
		return s_map;
	}

	// calls match(registration) for the registrations at physAddr and returns the view of the first one accepted
	template<typename TPredicate>
	LatteTextureView* FindFirst(MPTR physAddr, TPredicate&& match)
	{
		auto& map = RegistrationsByAddress();
		auto it = map.find(physAddr);
		if (it == map.end())
			return nullptr;
		for (const ViewRegistration& reg : it->second)
		{
			if (match(reg))
				return reg.view;
		}
		return nullptr;
	}
}

void LatteTextureViewLookupCache::Add(LatteTextureView* view, uint32 baseMip, uint32 baseSlice)
{
	LatteTexture* tex = view->baseTexture;
	MPTR physAddr = tex->physAddress;
	ViewRegistration reg;
	reg.width = tex->width;
	reg.height = tex->height;
	reg.pitch = tex->pitch;
	reg.firstMip = view->firstMip;
	reg.numMip = view->numMip;
	reg.firstSlice = view->firstSlice;
	reg.numSlice = view->numSlice;
	reg.format = view->format;
	reg.dim = view->dim;
	reg.isDepth = tex->isDepth;
	reg.view = view;
	if (baseMip != 0 || baseSlice != 0)
	{
		// register as if the subresource at (baseSlice, baseMip) were a texture of its own
		const sint32 mip = (sint32)baseMip;
		const sint32 slice = (sint32)baseSlice;
		cemu_assert_debug(mip >= 0 && slice >= 0);
		LatteTextureSliceMipInfo* sub = tex->GetSliceMipArrayEntry(slice, mip);
		physAddr = sub->addrStart;
		reg.pitch = sub->pitch;
		// width/height are derived from the texture format; a view with another format would need its own calculation
		cemu_assert_debug(reg.format == tex->format);
		reg.width = tex->GetMipWidth(mip);
		reg.height = tex->GetMipHeight(mip);
		cemu_assert(reg.firstMip >= mip);
		cemu_assert(reg.firstSlice >= slice);
		reg.firstMip -= mip;
		reg.firstSlice -= slice;
	}
	RegistrationsByAddress()[physAddr].push_back(reg);
	vectorAppendUnique(view->lookupAddresses, physAddr);
}

void LatteTextureViewLookupCache::RemoveAll(LatteTextureView* view)
{
	auto& map = RegistrationsByAddress();
	for (MPTR addr : view->lookupAddresses)
	{
		auto it = map.find(addr);
		if (it == map.end())
			continue;
		auto& list = it->second;
		list.erase(std::remove_if(list.begin(), list.end(), [view](const ViewRegistration& r) { return r.view == view; }), list.end());
		if (list.empty())
			map.erase(it);
	}
	view->lookupAddresses.clear();
}

LatteTextureView* LatteTextureViewLookupCache::lookup(MPTR physAddr, sint32 width, sint32 height, sint32 depth, sint32 pitch, sint32 firstMip, sint32 numMip, sint32 firstSlice, sint32 numSlice, Latte::E_GX2SURFFMT format, Latte::E_DIM dim)
{
	return FindFirst(physAddr, [&](const ViewRegistration& r) {
		return r.width == width && r.height == height && r.pitch == pitch && r.format == format && r.dim == dim &&
			r.firstMip == firstMip && r.numMip == numMip && r.firstSlice == firstSlice && r.numSlice == numSlice;
	});
}

LatteTextureView* LatteTextureViewLookupCache::lookupWithColorOrDepthType(MPTR physAddr, sint32 width, sint32 height, sint32 depth, sint32 pitch, sint32 firstMip, sint32 numMip, sint32 firstSlice, sint32 numSlice, Latte::E_GX2SURFFMT format, Latte::E_DIM dim, bool isDepth)
{
	cemu_assert_debug(firstSlice == 0);
	return FindFirst(physAddr, [&](const ViewRegistration& r) {
		return r.width == width && r.height == height && r.pitch == pitch && r.format == format && r.dim == dim &&
			r.firstMip == firstMip && r.numMip == numMip && r.firstSlice == firstSlice && r.numSlice == numSlice && r.isDepth == isDepth;
	});
}

LatteTextureView* LatteTextureViewLookupCache::lookupSlice(MPTR physAddr, sint32 width, sint32 height, sint32 pitch, sint32 firstMip, sint32 firstSlice, Latte::E_GX2SURFFMT format)
{
	return FindFirst(physAddr, [&](const ViewRegistration& r) {
		return r.width == width && r.height == height && r.pitch == pitch && r.format == format && r.firstSlice == firstSlice && r.firstMip == firstMip;
	});
}

LatteTextureView* LatteTextureViewLookupCache::lookupSliceMinSize(MPTR physAddr, sint32 minWidth, sint32 minHeight, sint32 pitch, sint32 firstMip, sint32 firstSlice, Latte::E_GX2SURFFMT format)
{
	return FindFirst(physAddr, [&](const ViewRegistration& r) {
		return r.width >= minWidth && r.height >= minHeight && r.pitch == pitch && r.format == format && r.firstSlice == firstSlice && r.firstMip == firstMip;
	});
}

LatteTextureView* LatteTextureViewLookupCache::lookupSliceEx(MPTR physAddr, sint32 width, sint32 height, sint32 pitch, sint32 firstMip, sint32 firstSlice, Latte::E_GX2SURFFMT format, bool isDepth)
{
	cemu_assert_debug(firstMip == 0);
	return FindFirst(physAddr, [&](const ViewRegistration& r) {
		return r.width == width && r.height == height && r.pitch == pitch && r.format == format && r.isDepth == isDepth && r.firstSlice == firstSlice && r.firstMip == firstMip;
	});
}

std::unordered_set<LatteTextureView*> LatteTextureViewLookupCache::GetAllViews()
{
	std::unordered_set<LatteTextureView*> result;
	for (const auto& [addr, list] : RegistrationsByAddress())
	{
		for (const ViewRegistration& reg : list)
			result.emplace(reg.view);
	}
	return result;
}
