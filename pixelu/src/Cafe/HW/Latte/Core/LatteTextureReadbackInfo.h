// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

// One asynchronous GPU->CPU copy of a texture subresource. Backends implement the
// transfer; the texture cache writes the received pixels back to guest memory.
class LatteTextureReadbackInfo
{
public:
	LatteTextureReadbackInfo(LatteTextureView* textureView, sint32 mipIndex = 0)
		: hostTextureCopy(textureView->baseTexture), m_firstSlice(textureView->firstSlice), m_firstMip(mipIndex), m_textureView(textureView)
	{}

	virtual ~LatteTextureReadbackInfo() = default;

	virtual void StartTransfer() = 0;
	virtual bool IsFinished() = 0;
	virtual void ForceFinish() {}

	virtual uint8* GetData() = 0;
	virtual void ReleaseData() {}

	HRTick transferStartTime;
	HRTick waitStartTime;
	bool forceFinish{ false }; // set when something had to wait for the transfer
	// description of the texture at the time the readback started (it may be deleted before completion)
	LatteTextureDefinition hostTextureCopy{};
	sint32 m_firstSlice{};
	sint32 m_firstMip{}; // mip level the backend transfers
	uint32 m_rowPitch = 0; // bytes per row in GetData(), set by the backend

protected:
	// tightly packed row size of the view's first mip, rounded up to rowAlignment (a power of two)
	static uint32 GetReadbackRowPitch(LatteTextureView* textureView, uint32 rowAlignment = 1);
	static uint32 GetReadbackImageSize(LatteTextureView* textureView, uint32 rowPitch);

	LatteTextureView* m_textureView;
	uint32 m_image_size = 0;
};
