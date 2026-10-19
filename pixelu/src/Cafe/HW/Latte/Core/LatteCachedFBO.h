// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "util/math/vector2.h"

// A framebuffer object for one combination of color and depth attachments. Renderers
// derive from it to hold their API object. The attachments are taken from the current
// LatteMRT state when the object is constructed (see LatteRenderTarget.cpp).
class LatteCachedFBO
{
public:
	LatteCachedFBO(uint64 key);
	virtual ~LatteCachedFBO() {};

	uint32 calculateNumColorBuffers()
	{
		uint32 count = 0;
		for (auto& slot : colorBuffer)
			count += slot.texture ? 1 : 0;
		return count;
	}

	bool hasDepthBuffer()
	{
		return depthBuffer.texture != nullptr;
	}

	// base textures of all attachments
	std::vector<LatteTexture*>& GetTextures()
	{
		return m_referencedTextures;
	}

private:
	// the drawable area is the smallest effective size of all attachments
	static void ShrinkToView(Vector2i& area, LatteTextureView* view)
	{
		sint32 w, h;
		view->baseTexture->GetEffectiveSize(w, h, view->firstMip);
		if (area.x == 0 && area.y == 0)
		{
			area = Vector2i(w, h);
			return;
		}
		if (w < area.x || h < area.y)
			cemuLog_logDebug(LogType::Force, "Framebuffer attachment is smaller than the others ({}x{} vs {}x{})", w, h, area.x, area.y);
		area.x = std::min(area.x, w);
		area.y = std::min(area.y, h);
	}

	void computeSize()
	{
		Vector2i area(0, 0);
		bool anyAttachment = false;
		for (auto& slot : colorBuffer)
		{
			if (!slot.texture)
				continue;
			ShrinkToView(area, slot.texture);
			anyAttachment = true;
		}
		if (depthBuffer.texture)
		{
			ShrinkToView(area, depthBuffer.texture);
			anyAttachment = true;
		}
		// without attachments the renderers still need some size
		m_size = anyAttachment ? area : Vector2i(32, 32);
		cemu_assert_debug(m_size.x != 0 && m_size.y != 0);
	}

public:
	uint64 key;
	Vector2i m_size;

	struct
	{
		LatteTextureView* texture{};
	}colorBuffer[8]{};
	struct
	{
		LatteTextureView* texture{};
		bool hasStencil{};
	}depthBuffer{};
	uint32 drawBuffersMask{};

	std::vector<LatteTexture*> m_referencedTextures; // in attachment order, color first
};

// The render target state: which textures are attached as color and depth buffers,
// derived from the GPU registers, and the cached FBOs for those combinations.
class LatteMRT
{
public:
	static void NotifyTextureDeletion(LatteTexture* texture);

	// attachments described by the GPU registers
	static LatteTextureView* GetColorAttachmentTexture(uint32 index, bool createNew, bool checkForTextureChanges);
	static uint8 GetActiveColorBufferMask(const LatteDecompilerShader* pixelShader, const struct LatteContextRegister& lcr);
	static bool GetActiveDepthBufferMask(const struct LatteContextRegister& lcr);
	static Latte::E_GX2SURFFMT GetColorBufferFormat(const uint32 index, const LatteContextRegister& lcr);
	static Latte::E_GX2SURFFMT GetDepthBufferFormat(const struct LatteContextRegister& lcr);

	// pending attachments, ApplyCurrentState binds them
	static void SetColorAttachment(uint32 index, LatteTextureView* view);
	static void SetDepthAndStencilAttachment(LatteTextureView* view, bool hasStencil);
	static LatteTextureView* GetColorAttachment(uint32 index);
	static LatteTextureView* GetDepthAttachment();

	static void ApplyCurrentState();

	// sets the attachments from the GPU registers, false if there is nothing to draw into
	static bool UpdateCurrentFBO();

	static void BindColorBufferOnly(LatteTextureView* view);
	static void BindDepthBufferOnly(LatteTextureView* view);

	static void GetCurrentFragCoordScale(float* coordScale);
	// size of the guest viewport, not scaled by graphic pack rules
	static void GetVirtualViewportDimensions(sint32& width, sint32& height);

	static void DeleteCachedFBO(LatteCachedFBO* cfbo);

private:
	static LatteCachedFBO* CreateCachedFBO(uint64 key);
};
