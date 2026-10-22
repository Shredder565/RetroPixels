// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"
#include "Common/GLInclude/GLInclude.h"

// GPU to CPU copy of one texture slice through a pixel pack buffer, completion is tracked with a fence
class LatteTextureReadbackInfoGL : public LatteTextureReadbackInfo
{
public:
	LatteTextureReadbackInfoGL(LatteTextureView* textureView);
	~LatteTextureReadbackInfoGL();

	void StartTransfer() override;
	bool IsFinished() override;
	void ForceFinish() override;

	uint8* GetData() override;
	void ReleaseData() override;

private:
	bool WaitForFence(GLuint64 timeoutNs);

	GLenum m_packFormat = 0;
	GLenum m_packType = 0;
	GLuint m_packBuffer = 0;
	GLsync m_fence = nullptr;
};
