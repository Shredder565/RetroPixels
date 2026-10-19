// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureViewGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureGL.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "config/LaunchSettings.h"

LatteTextureViewGL::LatteTextureViewGL(LatteTextureGL* texture, Latte::E_DIM dim, Latte::E_GX2SURFFMT format, sint32 firstMip, sint32 mipCount, sint32 firstSlice, sint32 sliceCount, bool registerView, bool forceCreateNewTexId)
	: LatteTextureView(texture, firstMip, mipCount, firstSlice, sliceCount, dim, format, registerView)
{
	const bool coversWholeTexture = dim == texture->dim && format == texture->format &&
		firstMip == 0 && firstSlice == 0 && mipCount == texture->mipLevels && sliceCount == texture->depth;
	if (coversWholeTexture && !forceCreateNewTexId)
	{
		glTexId = texture->glId_texture;
		glTexTarget = texture->glTexTarget;
		glInternalFormat = texture->glInternalFormat;
	}
	else
	{
		LatteTextureGL::GenerateEmptyTextureFromGX2Dim(dim, glTexId, glTexTarget, false);
		glInternalFormat = 0;
		InitAliasView();
	}

	// nothing has been applied to the GL texture yet, use values that never match a real setting
	samplerState.clampS = samplerState.clampT = samplerState.clampR = 0xFF;
	samplerState.filterMin = samplerState.filterMag = 0xFFFFFFFF;
	samplerState.maxAniso = 0xFF;
	samplerState.maxMipLevels = 0xFF;
	samplerState.borderType = 0xFF;
	for (float& c : samplerState.borderColor)
		c = 9999.0f;
	samplerState.minLod = 0xFFFF;
	samplerState.maxLod = 0xFFFF;
	samplerState.lodBias = 0x7FFF;
	samplerState.depthCompareMode = 0xFF;
	samplerState.depthCompareFunc = 0xFF;
	swizzleR = swizzleG = swizzleB = swizzleA = 0xFF;
}

LatteTextureViewGL::~LatteTextureViewGL()
{
	delete m_alternativeView;
	static_cast<OpenGLRenderer*>(g_renderer.get())->texture_notifyDelete(this);
	glDeleteTextures(1, &glTexId);
}

void LatteTextureViewGL::InitAliasView()
{
	auto* texture = static_cast<LatteTextureGL*>(baseTexture);
	// views of format-overwritten and depth textures keep the storage format, colour views may reinterpret it
	if (texture->overwriteInfo.hasFormatOverwrite)
	{
		cemu_assert_debug(format == texture->format);
		glInternalFormat = texture->glInternalFormat;
	}
	else if (texture->isDepth)
	{
		cemu_assert(format == texture->format);
		glInternalFormat = texture->glInternalFormat;
	}
	else
	{
		LatteTextureGL::FormatInfoGL info;
		LatteTextureGL::GetOpenGLFormatInfo(false, format, dim, &info);
		glInternalFormat = info.glInternalFormat;
	}

	catchOpenGLError();
	// a mip range past the allocated levels is clamped to the last level
	sint32 viewFirstMip = firstMip;
	if (viewFirstMip >= texture->maxPossibleMipLevels)
	{
		cemuLog_logDebug(LogType::Force, "LatteTextureViewGL: first mip {} is out of range", firstMip);
		viewFirstMip = texture->maxPossibleMipLevels - 1;
	}
	glTextureView(glTexId, glTexTarget, texture->glId_texture, glInternalFormat, viewFirstMip, numMip, firstSlice, numSlice);
	catchOpenGLError();

	if (glTextureParameteri)
	{
		glTextureParameteri(glTexId, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTextureParameteri(glTexId, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTextureParameteri(glTexId, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTextureParameteri(glTexId, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTextureParameteri(glTexId, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		glTextureParameteri(glTexId, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	}

	bool debugNames = LaunchSettings::NSightModeEnabled();
#ifdef CEMU_DEBUG_ASSERT
	debugNames = true;
#endif
	if (debugNames)
	{
		const std::string label = fmt::format("{:08x}_f{:04x}_p{:04x}_viewFMT{:04x}{}_org{}", texture->physAddress, (uint32)texture->format, texture->pitch, (uint32)format, texture->isDepth ? "_d" : "", texture->glId_texture);
		glObjectLabel(GL_TEXTURE, glTexId, -1, label.c_str());
	}
}

LatteTextureViewGL* LatteTextureViewGL::GetAlternativeView()
{
	if (m_alternativeView == nullptr)
		m_alternativeView = new LatteTextureViewGL(static_cast<LatteTextureGL*>(baseTexture), dim, format, firstMip, numMip, firstSlice, numSlice, false, true);
	return m_alternativeView;
}
