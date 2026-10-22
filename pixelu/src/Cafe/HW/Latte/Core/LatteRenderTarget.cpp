// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ISA/RegDefines.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "HW/Latte/Renderer/RendererCore.h"
#include "config/ActiveSettings.h"
#include "WindowSystem.h"
#include "Cafe/OS/libs/erreula/erreula.h"
#include "input/InputManager.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "Cafe/HW/Latte/Core/LatteRecorder.h"

// Render targets: turns the color/depth buffer registers into texture views, keeps the
// cached FBOs for each attachment combination, handles HLE clears and presents scan
// buffers to the TV and GamePad windows.

// last scissor rectangle passed to the renderer (the OpenGL renderer resets these)
uint32 prevScissorX = 0;
uint32 prevScissorY = 0;
uint32 prevScissorWidth = 0;
uint32 prevScissorHeight = 0;

// whether the last UpdateCurrentFBO found any color or depth buffer to draw into
bool hasValidFramebufferAttached = false;

namespace
{
	struct Extent
	{
		sint32 width;
		sint32 height;
	};

	// size information of the current draw target
	struct
	{
		Extent real; // size of the textures as the guest sees them
		Extent effective; // size after graphic pack resolution rules
		Extent guestViewport;
		bool resized; // real != effective
	}s_target{};

	// views written by the current draw, marked as GPU-modified afterwards
	struct
	{
		LatteTextureView* views[Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS + 1];
		sint32 count;
	}s_written{};

	// attachments for the next ApplyCurrentState
	struct
	{
		LatteTextureView* color[8]{};
		LatteTextureView* depth{};
		bool depthHasStencil{};
	}s_pending{};

	// bound when there is no attachment at all
	LatteCachedFBO* s_fboWithoutAttachments = nullptr;

	sint32 s_depthSizeWarnings = 0;

	// size of the image last presented, used for the aspect ratio of the window layout
	sint32 s_presentedWidth = 0;
	sint32 s_presentedHeight = 0;

	// the key identifies an attachment combination among the FBOs of one view
	uint64 AttachmentKey()
	{
		uint64 key = 0;
		for (LatteTextureView* view : s_pending.color)
		{
			if (view)
				key = std::rotl<uint64>(key + (uint64)view, 5);
			key = std::rotl<uint64>(key, 7);
		}
		if (s_pending.depth)
			key = std::rotl<uint64>(key + (uint64)s_pending.depth, 5) + (s_pending.depthHasStencil ? 1 : 0);
		return key;
	}

	// FBOs are stored in the list of the highest color attachment, or the depth view without color
	LatteTextureView* FBOOwnerView()
	{
		for (sint32 i = 7; i >= 0; i--)
		{
			if (s_pending.color[i])
				return s_pending.color[i];
		}
		return s_pending.depth;
	}

	template<typename T>
	void EraseValue(std::vector<T>& list, T value)
	{
		list.erase(std::remove(list.begin(), list.end(), value), list.end());
	}

	void DetachFBOFromView(LatteTextureView* view, LatteCachedFBO* fbo)
	{
		EraseValue(view->list_fboLookup, fbo);
		EraseValue(view->list_associatedFbo, fbo);
	}

	void TrackWrittenView(LatteTextureView* view)
	{
		s_written.views[s_written.count++] = view;
	}

	// color buffer pitch and height from CB_COLORn_SIZE (pitch in units of 8 pixels, area in units of 64)
	void DecodeColorBufferSize(uint32 sizeReg, uint32& pitch, uint32& height)
	{
		pitch = ((sizeReg & 0x3FF) + 1) * 8;
		const uint32 area = (((sizeReg >> 10) & 0xFFFFF) + 1) * 64;
		height = area / pitch;
	}

	// pointers of macro tiled surfaces carry the bank swizzle in bits 8-10
	uint32 SplitSwizzle(MPTR& addr, Latte::E_HWTILEMODE tileMode)
	{
		if (!Latte::TM_IsMacroTiled(tileMode))
			return 0;
		const uint32 swizzle = (addr >> 8) & 7;
		addr &= ~(7u << 8);
		return swizzle;
	}

	Latte::E_DIM DimForSliceCount(uint32 sliceCount)
	{
		return sliceCount > 1 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D;
	}

	LatteTextureView* CreateDepthBuffer(MPTR addr, uint32 width, uint32 height, uint32 pitch, Latte::E_HWTILEMODE tileMode, Latte::E_GX2SURFFMT format, uint32 swizzle, sint32 slice)
	{
		LatteTextureView* view = LatteTexture_CreateMapping(addr, MPTR_NULL, width, height, slice + 1, pitch, tileMode, swizzle, 0, 1, slice, 1, format, slice > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, true);
		LatteMRT::SetDepthAndStencilAttachment(view, view->baseTexture->hasStencil);
		return view;
	}

	enum class DepthResult
	{
		Attached,
		Detached,
		Unchanged, // valid size but no address: the previous depth attachment is kept
	};

	// the depth buffer from the DB registers; Detached if it can't be used for this draw
	DepthResult AcquireDepthBuffer(uint32 scissorRight, uint32 scissorBottom, LatteTextureView*& view)
	{
		view = nullptr;
		const uint32* regs = LatteGPUState.contextRegister;
		const uint32 sizeReg = regs[mmDB_DEPTH_SIZE];
		const uint32 infoReg = regs[mmDB_DEPTH_INFO];
		const Latte::E_GX2SURFFMT format = LatteMRT::GetDepthBufferFormat(LatteGPUState.contextNew);
		const Latte::E_HWTILEMODE tileMode = (Latte::E_HWTILEMODE)((infoReg >> 15) & 0xF);
		MPTR addr = regs[mmDB_HTILE_DATA_BASE] << 8;

		// pitch in units of 8 pixels, area in units of 64
		const uint32 pitchTiles = (sizeReg & 0x3FF) + 1;
		const uint32 pitch = pitchTiles * 8;
		uint32 height = ((((sizeReg >> 10) & 0xFFFFF) + 1) / pitchTiles) * 8;
		uint32 width = pitch;
		if (width < 2)
			width = 2;
		if (height < 2)
		{
			cemuLog_logDebug(LogType::Force, "Depth buffer height {} is invalid", height);
			height = 2;
		}

		// a depth buffer smaller than the scissor area is left out
		if (scissorRight > pitch || scissorBottom > height)
		{
			if (s_target.effective.width == 0 && s_target.effective.height == 0)
				s_target.effective = s_target.real;
			return DepthResult::Detached;
		}
		if (s_target.real.width == 0)
			s_target.real = { (sint32)width, (sint32)height };

		const uint32 viewReg = regs[mmDB_DEPTH_VIEW];
		const uint32 slice = viewReg & 0x7FF;
		cemu_assert_debug(((viewReg >> 13) & 0x7FF) == slice); // one slice only
		const uint32 swizzle = SplitSwizzle(addr, tileMode);
		if (addr == MPTR_NULL)
			return DepthResult::Unchanged;

		view = LatteTextureViewLookupCache::lookupSliceEx(addr, width, height, pitch, 0, slice, format, true);
		if (view)
		{
			LatteTexture_UpdateDataToLatest(view->baseTexture);
		}
		else
		{
			view = LatteTexture_CreateMapping(addr, 0, width, height, slice + 1, pitch, tileMode, swizzle, 0, 1, slice, 1, format, slice > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, true, true);
			LatteGPUState.repeatTextureInitialization = true;
		}

		sint32 effW, effH;
		view->baseTexture->GetEffectiveSize(effW, effH, view->firstMip);
		if (s_target.effective.width == 0 && s_target.effective.height == 0)
		{
			s_target.effective = { effW, effH };
		}
		else if (s_target.effective.width > effW && s_target.effective.height > effH && s_depthSizeWarnings < 100)
		{
			s_depthSizeWarnings++;
			cemuLog_logDebug(LogType::Force, "Depth buffer {:08x} ({}x{}) is smaller than the color buffers (effective {}x{})", view->baseTexture->physAddress, view->baseTexture->width, view->baseTexture->height, effW, effH);
		}
		LatteTC_MarkTextureStillInUse(view->baseTexture);
		return DepthResult::Attached;
	}

	void ClearAsColor(LatteTexture* texture, uint32 slice, uint32 mip, float r, float g, float b, float a, uint64 eventCounter)
	{
		if (texture->isDepth)
		{
			cemuLog_logDebug(LogType::Force, "Color clear of a depth texture is not supported");
			return;
		}
		g_renderer->texture_clearColorSlice(texture, slice, mip, r, g, b, a);
		LatteTexture_MarkDynamicTextureAsChanged(texture->baseView, slice, mip, eventCounter);
	}

	void ClearAsDepth(LatteTexture* texture, uint32 slice, uint32 mip, bool depth, bool stencil, float depthValue, uint8 stencilValue, uint64 eventCounter)
	{
		if (texture->isDepth)
		{
			g_renderer->texture_clearDepthSlice(texture, slice, mip, depth, stencil, depthValue, stencilValue);
		}
		else
		{
			// a stencil clear on a color texture is not meant for it
			if (stencil)
				return;
			if (depth)
				g_renderer->texture_clearColorSlice(texture, slice, mip, depthValue, depthValue, depthValue, depthValue);
		}
		LatteTexture_MarkDynamicTextureAsChanged(texture->baseView, slice, mip, eventCounter);
	}

	// built-in output shader for the configured scaling filter
	RendererOutputShader* BuiltinOutputShader(bool downscaling, bool upsideDown, LatteTextureView::MagFilter& filter)
	{
		const sint32 setting = downscaling ? GetConfig().downscale_filter : GetConfig().upscale_filter;
		filter = LatteTextureView::MagFilter::kLinear;
		switch (setting)
		{
		case kLinearFilter:
			return upsideDown ? RendererOutputShader::s_copy_shader_ud : RendererOutputShader::s_copy_shader;
		case kBicubicFilter:
			return upsideDown ? RendererOutputShader::s_bicubic_shader_ud : RendererOutputShader::s_bicubic_shader;
		case kBicubicHermiteFilter:
			return upsideDown ? RendererOutputShader::s_hermit_shader_ud : RendererOutputShader::s_hermit_shader;
		case kNearestNeighborFilter:
			filter = LatteTextureView::MagFilter::kNearestNeighbor;
			return upsideDown ? RendererOutputShader::s_copy_shader_ud : RendererOutputShader::s_copy_shader;
		default:
			return nullptr;
		}
	}

	// the first graphic pack with a matching scaling shader or a general output shader wins
	RendererOutputShader* OutputShader(bool downscaling, bool upsideDown, LatteTextureView::MagFilter& filter)
	{
		for (const auto& pack : GraphicPack2::GetActiveGraphicPacks())
		{
			RendererOutputShader* scaling = downscaling ? pack->GetDownscalingShader(upsideDown) : pack->GetUpscalingShader(upsideDown);
			RendererOutputShader* chosen = scaling ? scaling : pack->GetOuputShader(upsideDown);
			if (chosen)
			{
				filter = downscaling ? pack->GetDownscalingMagFilter() : pack->GetUpscalingMagFilter();
				return chosen;
			}
		}
		return BuiltinOutputShader(downscaling, upsideDown, filter);
	}

	// GamePad screen button of an emulated controller: held (show the other screen) or toggle
	struct ScreenButton
	{
		bool held = false;
		bool toggle = false;
	};

	ScreenButton ReadScreenButton(size_t controllerIndex)
	{
		ScreenButton result;
		auto controller = InputManager::instance().get_vpad_controller(controllerIndex);
		if (!controller)
			return result;
		const bool active = controller->is_screen_active();
		const bool isToggle = controller->is_screen_active_toggle();
		result.held = active && !isToggle;
		result.toggle = active && isToggle;
		return result;
	}

	float ScaleX(float v)
	{
		return v * ((float)s_target.effective.width / (float)s_target.real.width);
	}

	float ScaleY(float v)
	{
		return v * ((float)s_target.effective.height / (float)s_target.real.height);
	}
}

LatteCachedFBO::LatteCachedFBO(uint64 key) : key(key)
{
	for (sint32 i = 0; i < 8; i++)
	{
		LatteTextureView* view = s_pending.color[i];
		colorBuffer[i].texture = view;
		if (!view)
			continue;
		vectorAppendUnique(view->list_associatedFbo, this);
		m_referencedTextures.emplace_back(view->baseTexture);
	}
	if (LatteTextureView* view = s_pending.depth)
	{
		depthBuffer.texture = view;
		depthBuffer.hasStencil = s_pending.depthHasStencil;
		vectorAppendUnique(view->list_associatedFbo, this);
		m_referencedTextures.emplace_back(view->baseTexture);
	}
	computeSize();
}

void LatteMRT::NotifyTextureDeletion(LatteTexture* texture)
{
	// only the color attachments are dropped here
	for (LatteTextureView*& view : s_pending.color)
	{
		if (view && view->baseTexture == texture)
			view = nullptr;
	}
}

LatteCachedFBO* LatteMRT::CreateCachedFBO(uint64 key)
{
	return g_renderer->rendertarget_createCachedFBO(key);
}

void LatteMRT::DeleteCachedFBO(LatteCachedFBO* cfbo)
{
	for (auto& slot : cfbo->colorBuffer)
	{
		if (slot.texture)
			DetachFBOFromView(slot.texture, cfbo);
	}
	if (cfbo->depthBuffer.texture)
		DetachFBOFromView(cfbo->depthBuffer.texture, cfbo);
	g_renderer->rendertarget_deleteCachedFBO(cfbo);
	delete cfbo;
}

void LatteMRT::SetColorAttachment(uint32 index, LatteTextureView* view)
{
	s_pending.color[index] = view;
}

void LatteMRT::SetDepthAndStencilAttachment(LatteTextureView* view, bool hasStencil)
{
	s_pending.depth = view;
	s_pending.depthHasStencil = hasStencil;
}

LatteTextureView* LatteMRT::GetColorAttachment(uint32 index)
{
	cemu_assert_debug(index < 8);
	return s_pending.color[index];
}

LatteTextureView* LatteMRT::GetDepthAttachment()
{
	return s_pending.depth;
}

// binds the FBO for the pending attachments, creating it on first use
void LatteMRT::ApplyCurrentState()
{
	const uint64 key = AttachmentKey();
	LatteTextureView* owner = FBOOwnerView();
	if (!owner)
	{
		if (!s_fboWithoutAttachments)
			s_fboWithoutAttachments = CreateCachedFBO(key);
		g_renderer->rendertarget_bindFramebufferObject(s_fboWithoutAttachments);
		return;
	}
	for (LatteCachedFBO* fbo : owner->list_fboLookup)
	{
		if (fbo->key != key)
			continue;
		g_renderer->rendertarget_bindFramebufferObject(fbo);
		return;
	}
	LatteCachedFBO* fbo = CreateCachedFBO(key);
	g_renderer->rendertarget_bindFramebufferObject(fbo);
	owner->list_fboLookup.push_back(fbo);
	cemu_assert_debug(fbo->depthBuffer.texture == s_pending.depth);
}

void LatteMRT::BindColorBufferOnly(LatteTextureView* view)
{
	cemu_assert_debug(!view->baseTexture->isDepth);
	for (uint32 i = 0; i < 8; i++)
		SetColorAttachment(i, i == 0 ? view : nullptr);
	SetDepthAndStencilAttachment(nullptr, false);
	ApplyCurrentState();
}

void LatteMRT::BindDepthBufferOnly(LatteTextureView* view)
{
	cemu_assert_debug(view->baseTexture->isDepth);
	for (uint32 i = 0; i < 8; i++)
		SetColorAttachment(i, nullptr);
	SetDepthAndStencilAttachment(view, view->baseTexture->hasStencil);
	ApplyCurrentState();
}

// the view for color buffer `index` as set up in the CB registers, created if needed
LatteTextureView* LatteMRT::GetColorAttachmentTexture(uint32 index, bool createNew, bool checkForTextureChanges)
{
	const uint32* regs = LatteGPUState.contextRegister;
	const uint32 sizeReg = regs[mmCB_COLOR0_SIZE + index];
	const uint32 infoReg = regs[mmCB_COLOR0_INFO + index];
	const uint32 viewReg = regs[mmCB_COLOR0_VIEW + index];
	const Latte::E_HWTILEMODE tileMode = (Latte::E_HWTILEMODE)((infoReg >> 8) & 0xF);
	const Latte::E_GX2SURFFMT format = GetColorBufferFormat(index, LatteGPUState.contextNew);

	// the low byte of the base address is not used
	MPTR addr = regs[mmCB_COLOR0_BASE + index] & 0xFFFFFF00;
	uint32 swizzleBits = 0; // kept in register position (bits 8-10)
	if (Latte::TM_IsMacroTiled(tileMode))
	{
		swizzleBits = addr & 0x700;
		addr &= ~0x700u;
	}

	const uint32 firstSlice = viewReg & 0x7FF;
	const uint32 sliceCount = ((viewReg >> 13) & 0x7FF) - firstSlice + 1;
	if (sliceCount != 1)
		cemuLog_logDebug(LogType::Force, "Color buffer view with {} slices", sliceCount);

	uint32 pitch, height;
	DecodeColorBufferSize(sizeReg, pitch, height);
	uint32 width = pitch;
	// the registers hold sizes padded to 8x32; the scissor box often gives the real size
	if (LatteGPUState.allowFramebufferSizeOptimization)
	{
		const uint32 scissorRight = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_X();
		const uint32 scissorBottom = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();
		if (((scissorRight + 7) & ~7u) == width)
			width = scissorRight;
		if ((height & 31) == 0)
			height = scissorBottom;
	}

	const sint32 firstMip = 0;
	LatteTextureView* view = LatteTextureViewLookupCache::lookupSliceEx(addr, width, height, pitch, firstMip, firstSlice, format, false);
	if (!view)
	{
		const uint32 sliceEnd = firstSlice + sliceCount;
		view = LatteTexture_CreateMapping(addr, 0, width, height, sliceEnd, pitch, tileMode, swizzleBits >> 8, firstMip, 1, firstSlice, sliceCount, format, DimForSliceCount(sliceEnd), Latte::E_DIM::DIM_2D, false, true);
		LatteGPUState.repeatTextureInitialization = true;
		checkForTextureChanges = false; // new textures already hold the latest data
	}
	// note: compares the stored swizzle against the register-position bits
	if (view->baseTexture->swizzle != swizzleBits)
		view->baseTexture->lastRenderTargetSwizzle = swizzleBits;
	if (checkForTextureChanges)
		LatteTexture_UpdateDataToLatest(view->baseTexture);
	LatteTC_MarkTextureStillInUse(view->baseTexture);
	return view;
}

// color buffers the pixel shader writes and that can hold the scissor area
uint8 LatteMRT::GetActiveColorBufferMask(const LatteDecompilerShader* pixelShader, const LatteContextRegister& lcr)
{
	if (!pixelShader) [[unlikely]]
		return 0;
	if (lcr.CB_COLOR_CONTROL.get_SPECIAL_OP() == Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE)
		return 0;
	cemu_assert_debug(!lcr.CB_COLOR_CONTROL.get_DEGAMMA_ENABLE());
	cemu_assert_debug(lcr.PA_SC_GENERIC_SCISSOR_TL.get_WINDOW_OFFSET_DISABLE());

	const uint32* regs = lcr.GetRawView();
	const uint32 channelMask = lcr.CB_TARGET_MASK.get_MASK();
	const uint32 scissorRight = lcr.PA_SC_GENERIC_SCISSOR_BR.get_BR_X();
	const uint32 scissorBottom = lcr.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();
	uint8 mask = pixelShader->pixelColorOutputMask;
	for (uint32 i = 0; i < 8; i++)
	{
		const uint8 bit = (uint8)(1 << i);
		if (((channelMask >> (i * 4)) & 0xF) == 0)
			mask &= ~bit; // all channels write-protected
		if ((mask & bit) == 0)
			continue;
		uint32 pitch, height;
		DecodeColorBufferSize(regs[mmCB_COLOR0_SIZE + i], pitch, height);
		const bool noAddress = regs[mmCB_COLOR0_BASE + i] == MPTR_NULL;
		if (noAddress || pitch < scissorRight || height < scissorBottom)
			mask &= ~bit;
	}
	return mask;
}

// the depth buffer is attached only when depth or stencil testing is on
bool LatteMRT::GetActiveDepthBufferMask(const LatteContextRegister& lcr)
{
	const auto& control = lcr.DB_DEPTH_CONTROL;
	return control.get_Z_ENABLE() || control.get_STENCIL_ENABLE() || control.get_BACK_STENCIL_ENABLE();
}

Latte::E_GX2SURFFMT LatteMRT::GetColorBufferFormat(const uint32 index, const LatteContextRegister& lcr)
{
	cemu_assert_debug(index < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS);
	// CB number type -> GX2 surface format flag bits (unorm/snorm/uint/sint/float/srgb)
	static constexpr uint32 kNumberTypeBits[8] = { 0, 0x200, 0, 0, 0x100, 0x300, 0x400, 0x800 };
	const uint32 info = lcr.GetRawView()[mmCB_COLOR0_INFO + index];
	return (Latte::E_GX2SURFFMT)(((info >> 2) & 0x3F) | kNumberTypeBits[(info >> 12) & 7]);
}

Latte::E_GX2SURFFMT LatteMRT::GetDepthBufferFormat(const LatteContextRegister& lcr)
{
	const uint32 hwFormat = lcr.GetRawView()[mmDB_DEPTH_INFO] & 7;
	switch (hwFormat)
	{
	case 1: return Latte::E_GX2SURFFMT::D16_UNORM;
	case 3: return Latte::E_GX2SURFFMT::D24_S8_UNORM;
	case 5: return Latte::E_GX2SURFFMT::D24_S8_FLOAT;
	case 6: return Latte::E_GX2SURFFMT::D32_FLOAT;
	case 7: return Latte::E_GX2SURFFMT::D32_S8_FLOAT;
	default: break;
	}
	cemuLog_logDebug(LogType::Force, "DB_DEPTH_INFO has unknown depth format {}", hwFormat);
	return Latte::E_GX2SURFFMT::D16_UNORM;
}

// attaches the buffers of the current GPU state, false if the target has no usable size
bool LatteMRT::UpdateCurrentFBO()
{
	s_written.count = 0;
	const uint8 colorMask = GetActiveColorBufferMask(LatteSHRC_GetActivePixelShader(), LatteGPUState.contextNew);
	const bool useDepth = GetActiveDepthBufferMask(LatteGPUState.contextNew);
	s_target.resized = false;
	s_target.real = { 0, 0 };
	s_target.effective = { 0, 0 };

	cemu_assert_debug(LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_WINDOW_OFFSET_DISABLE());
	const uint32 scissorRight = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_X();
	const uint32 scissorBottom = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();

	for (uint32 i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
	{
		if ((colorMask & (1 << i)) == 0)
		{
			SetColorAttachment(i, nullptr);
			continue;
		}
		LatteTextureView* view = GetColorAttachmentTexture(i, true, true);
		SetColorAttachment(i, view);
		TrackWrittenView(view);
		LatteTexture* tex = view->baseTexture;
		sint32 effW, effH;
		tex->GetEffectiveSize(effW, effH, view->firstMip);
		if (s_target.effective.width == 0 && s_target.effective.height == 0)
			s_target.effective = { effW, effH };
		else if (s_target.effective.width != effW && s_target.effective.height != effH)
			cemuLog_logDebug(LogType::Force, "Color buffer {:08x} ({}x{} fmt {:04x}) has effective size {}x{}, others have {}x{}", tex->physAddress, tex->width, tex->height, (uint32)tex->format, effW, effH, s_target.effective.width, s_target.effective.height);
		// the first color buffer decides the size of the render target
		if (s_target.real.width == 0 && s_target.real.height == 0)
		{
			sint32 w, h;
			tex->GetSize(w, h, view->firstMip);
			s_target.real = { w, h };
		}
	}

	LatteTextureView* depthView = nullptr;
	const DepthResult depthResult = useDepth ? AcquireDepthBuffer(scissorRight, scissorBottom, depthView) : DepthResult::Detached;
	if (depthResult == DepthResult::Attached)
	{
		TrackWrittenView(depthView);
		SetDepthAndStencilAttachment(depthView, depthView->baseTexture->hasStencil);
	}
	else if (depthResult == DepthResult::Detached)
	{
		SetDepthAndStencilAttachment(nullptr, false);
	}

	hasValidFramebufferAttached = colorMask != 0 || useDepth;
	if (!hasValidFramebufferAttached)
		return true;
	s_target.resized = s_target.effective.width != s_target.real.width || s_target.effective.height != s_target.real.height;
	if (s_target.effective.width == 0)
	{
		cemuLog_logDebug(LogType::Force, "Render target has no size, the color/depth buffer setup is probably invalid");
		return false;
	}
	return true;
}

// xy: real-to-effective scale of the render target, zw: guest viewport size
void LatteMRT::GetCurrentFragCoordScale(float* coordScale)
{
	coordScale[0] = 1.0f;
	coordScale[1] = 1.0f;
	if (s_target.resized)
	{
		coordScale[0] = (float)s_target.real.width / (float)s_target.effective.width;
		coordScale[1] = (float)s_target.real.height / (float)s_target.effective.height;
	}
	coordScale[2] = (float)s_target.guestViewport.width;
	coordScale[3] = (float)s_target.guestViewport.height;
}

void LatteMRT::GetVirtualViewportDimensions(sint32& width, sint32& height)
{
	width = s_target.guestViewport.width;
	height = s_target.guestViewport.height;
}

// after a draw: mark the written views as changed on the GPU and start readbacks
void LatteRenderTarget_trackUpdates()
{
	const uint64 eventCounter = LatteTexture_getNextUpdateEventCounter();
	for (sint32 i = 0; i < s_written.count; i++)
	{
		LatteTextureView* view = s_written.views[i];
		LatteTexture_TrackTextureGPUWrite(view->baseTexture, view->firstSlice, view->firstMip, eventCounter);
		if (view->baseTexture->enableReadback)
			LatteTextureReadback_Initate(view);
	}
}

// end of a TV frame
void LatteRenderTarget_itHLESwapScanBuffer()
{
	performanceMonitor.cycle[performanceMonitor.cycleIndex].frameCounter++;
	if (LatteGPUState.frameCounter > 5)
		performanceMonitor.gpuTime_frameTime.endMeasuring();
	LattePerformanceMonitor_frameEnd();
	FrameBenchmarkHelper::FrameEnd();
	LatteGPUState.frameCounter++;
	g_renderer->SwapBuffers(true, true);
	performanceMonitor.gpuTime_frameTime.beginMeasuring();

	// per-frame housekeeping
	LatteTC_CleanupUnusedTextures();
	LatteSHRC_CleanupShaderStateCache();
#ifdef ENABLE_OPENGL
	LatteDraw_cleanupAfterFrame();
#endif
	LatteQuery_CancelActiveGPU7Queries();
	LatteBufferCache_notifySwapTVScanBuffer();
	LattePerformanceMonitor_frameBegin();
}

// GX2ClearColor/GX2ClearDepthStencil: clears every texture that maps the given buffers
// and creates a matching texture if none exists yet. clearMask: 1 color, 2 depth, 4 stencil
void LatteRenderTarget_itHLEClearColorDepthStencil(uint32 clearMask,
	MPTR colorBufferMPTR, Latte::E_GX2SURFFMT colorBufferFormat, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferPitch, uint32 colorBufferViewFirstSlice, uint32 colorBufferViewNumSlice,
	MPTR depthBufferMPTR, Latte::E_GX2SURFFMT depthBufferFormat, Latte::E_HWTILEMODE depthBufferTileMode, sint32 depthBufferWidth, sint32 depthBufferHeight, sint32 depthBufferPitch, sint32 depthBufferViewFirstSlice, sint32 depthBufferViewNumSlice,
	float r, float g, float b, float a, float clearDepth, uint32 clearStencil)
{
	const bool clearColor = (clearMask & 1) != 0;
	const bool clearDepthValue = (clearMask & 2) != 0;
	const bool clearStencilValue = (clearMask & 4) != 0;
	const uint32 mip = 0; // mip clears are not supported
	const uint32 colorSwizzle = SplitSwizzle(colorBufferMPTR, colorBufferTilemode);
	const uint32 depthSwizzle = SplitSwizzle(depthBufferMPTR, depthBufferTileMode);
	cemu_assert_debug(colorBufferViewNumSlice <= 1 && depthBufferViewNumSlice <= 1);

	const uint64 eventCounter = LatteTexture_getNextUpdateEventCounter();
	if (clearColor && colorBufferMPTR != MPTR_NULL && colorBufferWidth > 0 && colorBufferHeight > 0)
	{
		bool exactMatch = false;
		sint32 cursor = 0;
		const auto formatBits = Latte::GetFormatBits(colorBufferFormat);
		while (LatteTextureView* view = LatteTC_LookupTextureByData(colorBufferMPTR, colorBufferWidth, colorBufferHeight, colorBufferPitch, 0, 1, colorBufferViewFirstSlice, 1, &cursor))
		{
			LatteTexture* tex = view->baseTexture;
			if (Latte::GetFormatBits(tex->format) != formatBits)
				continue;
			exactMatch |= (uint32)tex->pitch == colorBufferPitch && (uint32)tex->height == colorBufferHeight;
			ClearAsColor(tex, colorBufferViewFirstSlice, mip, r, g, b, a, eventCounter);
		}
		if (!exactMatch)
		{
			const uint32 slice = colorBufferViewFirstSlice;
			LatteTextureView* created = LatteTexture_CreateMapping(colorBufferMPTR, MPTR_NULL, colorBufferWidth, colorBufferHeight, slice + 1, colorBufferPitch, colorBufferTilemode, colorSwizzle, 0, 1, slice, 1, colorBufferFormat, slice > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, false);
			ClearAsColor(created->baseTexture, slice, mip, r, g, b, a, eventCounter);
		}
	}

	if ((clearDepthValue || clearStencilValue) && depthBufferMPTR != MPTR_NULL)
	{
		std::vector<LatteTexture*> candidates;
		LatteTC_LookupTexturesByPhysAddr(depthBufferMPTR, candidates);
		bool exactMatch = false;
		for (LatteTexture* tex : candidates)
		{
			// textures wider than the buffer or without the slice are left alone
			if (tex->pitch > depthBufferPitch || depthBufferViewFirstSlice >= tex->depth)
				continue;
			exactMatch |= tex->pitch == depthBufferPitch && tex->height == depthBufferHeight;
			ClearAsDepth(tex, depthBufferViewFirstSlice, mip, clearDepthValue, clearStencilValue, clearDepth, clearStencil, eventCounter);
		}
		if (!exactMatch)
		{
			LatteTextureView* created = CreateDepthBuffer(depthBufferMPTR, depthBufferWidth, depthBufferHeight, depthBufferPitch, depthBufferTileMode, depthBufferFormat, depthSwizzle, depthBufferViewFirstSlice);
			ClearAsDepth(created->baseTexture, depthBufferViewFirstSlice, mip, clearDepthValue, clearStencilValue, clearDepth, clearStencil, eventCounter);
		}
	}
}

// where the presented image goes inside the TV (or GamePad) window
void LatteRenderTarget_getScreenImageArea(sint32* x, sint32* y, sint32* width, sint32* height, sint32* fullWidth, sint32* fullHeight, bool padView)
{
	int winW, winH;
	if (padView && WindowSystem::IsPadWindowOpen())
		WindowSystem::GetPadWindowPhysSize(winW, winH);
	else
		WindowSystem::GetWindowPhysSize(winW, winH);

	sint32 outW = winW;
	sint32 outH = winH;
	if (GetConfig().fullscreen_scaling == kKeepAspectRatio)
	{
		// fit to the window width, or to the height if that is too tall
		outH = s_presentedHeight * winW / std::max(s_presentedWidth, 1);
		if (outH > winH)
		{
			outW = s_presentedWidth * winH / std::max(s_presentedHeight, 1);
			outH = winH;
		}
	}
	*x = (winW - outW) / 2;
	*y = (winH - outH) / 2;
	*width = outW;
	*height = outH;
	if (fullWidth)
		*fullWidth = winW;
	if (fullHeight)
		*fullHeight = winH;
}

// draws the texture into the TV or GamePad window and renders the overlays on top
void LatteRenderTarget_copyToBackbuffer(LatteTextureView* textureView, bool isPadView)
{
	LatteTexture* tex = textureView->baseTexture;
	LatteTexture_UpdateDataToLatest(tex);
	LatteTC_MarkTextureStillInUse(tex);

	sint32 srcW, srcH;
	tex->GetEffectiveSize(srcW, srcH, 0);
	s_presentedWidth = srcW;
	s_presentedHeight = srcH;

	sint32 dstX, dstY, dstW, dstH, winW, winH;
	LatteRenderTarget_getScreenImageArea(&dstX, &dstY, &dstW, &dstH, &winW, &winH, isPadView);
	const bool letterboxed = dstW != winW || dstH != winH;

	// bicubic and similar filters are only used when the image gets bigger
	const bool downscaling = dstW <= srcW || dstH <= srcH;
	LatteTextureView::MagFilter filter = LatteTextureView::MagFilter::kLinear;
	RendererOutputShader* shader = OutputShader(downscaling, ActiveSettings::RenderUpsideDownEnabled(), filter);
	cemu_assert(shader);

	g_renderer->DrawBackbufferQuad(textureView, shader, filter == LatteTextureView::MagFilter::kLinear, dstX, dstY, dstW, dstH, isPadView, letterboxed);
	g_renderer->HandleScreenshotRequest(textureView, isPadView);
	if (!g_renderer->ImguiBegin(!isPadView))
		return;
	swkbd_render(!isPadView);
	nn::erreula::render(!isPadView);
	LatteOverlay_render(isPadView);
	g_renderer->ImguiEnd();
}

// GX2CopyColorBufferToScanBuffer: presents a color buffer on the TV and/or GamePad
void LatteRenderTarget_itHLECopyColorBufferToScanBuffer(MPTR colorBufferPtr, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferSliceIndex, uint32 colorBufferFormat, uint32 colorBufferPitch, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferSwizzle, uint32 renderTarget)
{
	cemu_assert_debug(colorBufferSliceIndex == 0);
	LatteTextureView* texView = LatteTC_GetTextureSliceViewOrTryCreate(colorBufferPtr, MPTR_NULL, (Latte::E_GX2SURFFMT)colorBufferFormat, colorBufferTilemode, colorBufferWidth, colorBufferHeight, 1, colorBufferPitch, colorBufferSwizzle, 0, 0, true);
	if (!texView)
		return;

	// Tab shows the other screen while held, Ctrl+Tab (or a controller toggle) swaps them
	const bool tabPressed = WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::TAB);
	const bool ctrlPressed = WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::LCONTROL);
	const ScreenButton vpad0 = ReadScreenButton(0);
	const ScreenButton vpad1 = ReadScreenButton(1);
	const bool vpad0Active = vpad0.held, vpad0Toggle = vpad0.toggle;
	const bool vpad1Active = vpad1.held, vpad1Toggle = vpad1.toggle;

	const bool altScreenRequested = (!ctrlPressed && tabPressed) || vpad0Active || vpad1Active;
	const bool togglePressed = (ctrlPressed && tabPressed) || vpad0Toggle || vpad1Toggle;
	static bool togglePressedLast = false;

	bool& isDRCPrimary = LatteGPUState.isDRCPrimary;
	if (togglePressed && !togglePressedLast)
		isDRCPrimary = !isDRCPrimary;
	togglePressedLast = togglePressed;

	// the main window shows the GamePad image when it is primary xor the other screen is held
	const bool showDRC = swkbd_hasKeyboardInputHook() == false && (isDRCPrimary ^ altScreenRequested);

	// PixelU recorder: always the game's TV picture, independent of what the windows show and before any overlay
	if ((renderTarget & RENDER_TARGET_TV) && LatteRecorder::IsRecording())
		LatteRecorder::OnTVScanBufferCopy(texView);

	if ((renderTarget & RENDER_TARGET_DRC) && g_renderer->IsPadWindowActive())
		LatteRenderTarget_copyToBackbuffer(texView, true);
	if (((renderTarget & RENDER_TARGET_TV) && !showDRC) || ((renderTarget & RENDER_TARGET_DRC) && showDRC))
		LatteRenderTarget_copyToBackbuffer(texView, false);
}

// guest viewport size, unaffected by graphic pack resolution rules
void LatteRenderTarget_GetCurrentVirtualViewportSize(sint32* viewportWidth, sint32* viewportHeight)
{
	*viewportWidth = s_target.guestViewport.width;
	*viewportHeight = s_target.guestViewport.height;
}

// viewport from the PA_CL_VPORT scale/offset registers, scaled to the effective render target size
void LatteRenderTarget_updateViewport()
{
	const auto& ctx = LatteGPUState.contextNew;
	const float xScale = ctx.PA_CL_VPORT_XSCALE.get_SCALE();
	const float yScale = ctx.PA_CL_VPORT_YSCALE.get_SCALE();
	float x = ctx.PA_CL_VPORT_XOFFSET.get_OFFSET() - xScale;
	float y = ctx.PA_CL_VPORT_YOFFSET.get_OFFSET() + yScale;
	float width = xScale / 0.5f;
	float height = yScale / -0.5f;

	// D3D style clip space maps z to [0,1], GL style to [-1,1]
	const bool halfZ = ctx.PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF();
	const float zScale = ctx.PA_CL_VPORT_ZSCALE.get_SCALE();
	const float zOffset = ctx.PA_CL_VPORT_ZOFFSET.get_OFFSET();
	const float farZ = zOffset + zScale;
	const float nearZ = halfZ ? zOffset : zOffset - zScale;

	s_target.guestViewport.width = (sint32)width;
	s_target.guestViewport.height = (sint32)height;
	if (s_target.resized)
	{
		x = ScaleX(x);
		y = ScaleY(y);
		width = ScaleX(width);
		height = ScaleY(height);
	}
	g_renderer->renderTarget_setViewport(x, y, width, height, nearZ, farZ, halfZ);
}

// scissor from PA_SC_GENERIC_SCISSOR, only passed on when it changed
void LatteRenderTarget_updateScissorBox()
{
	const auto& ctx = LatteGPUState.contextNew;
	uint32 left = ctx.PA_SC_GENERIC_SCISSOR_TL.get_TL_X();
	uint32 top = ctx.PA_SC_GENERIC_SCISSOR_TL.get_TL_Y();
	uint32 width = ctx.PA_SC_GENERIC_SCISSOR_BR.get_BR_X() - left;
	uint32 height = ctx.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y() - top;
	if (s_target.resized)
	{
		left = (uint32)(sint32)ScaleX((float)left);
		top = (uint32)(sint32)ScaleY((float)top);
		width = (uint32)(sint32)ScaleX((float)width);
		height = (uint32)(sint32)ScaleY((float)height);
	}
	const bool changed = left != prevScissorX || top != prevScissorY || width != prevScissorWidth || height != prevScissorHeight;
	if (!changed)
		return;
	g_renderer->renderTarget_setScissor(left, top, width, height);
	prevScissorX = left;
	prevScissorY = top;
	prevScissorWidth = width;
	prevScissorHeight = height;
}

void LatteRenderTarget_unloadAll()
{
	if (!s_fboWithoutAttachments)
		return;
	LatteMRT::DeleteCachedFBO(s_fboWithoutAttachments);
	s_fboWithoutAttachments = nullptr;
}
