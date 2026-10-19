// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Common/GLInclude/GLInclude.h"

class LatteTextureViewGL : public LatteTextureView
{
public:
	// a view that covers the whole texture with the same dim and format shares the texture's GL name,
	// any other view (or forceCreateNewTexId) gets its own glTextureView
	LatteTextureViewGL(class LatteTextureGL* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount, bool registerView = true, bool forceCreateNewTexId = false);
	~LatteTextureViewGL();

	// second GL view of the same range with its own sampler state, used when one texture is bound to two units with different settings
	LatteTextureViewGL* GetAlternativeView();

	// cached GL texture parameters, 0xFF/0xFFFFFFFF/... means not set yet
	LatteSamplerState samplerState{};
	uint8 swizzleR;
	uint8 swizzleG;
	uint8 swizzleB;
	uint8 swizzleA;

	GLuint glTexId;
	GLint glTexTarget;
	sint32 glInternalFormat;

	LatteTextureViewGL* m_alternativeView{ nullptr };

private:
	void InitAliasView();
};
