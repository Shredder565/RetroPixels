// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Renderer/OpenGL/LatteTextureViewGL.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Common/GLInclude/GLInclude.h"

// OpenGL framebuffer object for one combination of colour/depth attachments
class CachedFBOGL : public LatteCachedFBO
{
public:
	CachedFBOGL(uint64 key) : LatteCachedFBO(key)
	{
		glGenFramebuffers(1, &glId_fbo);
		// attachments are set on the bound framebuffer
		((OpenGLRenderer*)g_renderer.get())->rendertarget_bindFramebufferObject(this);
		for (sint32 slot = 0; slot < 8; slot++)
			AttachColor(slot, (LatteTextureViewGL*)colorBuffer[slot].texture);
		AttachDepthStencil((LatteTextureViewGL*)depthBuffer.texture, depthBuffer.hasStencil);
		SelectDrawBuffers();
	}

	GLuint glId_fbo{};

private:
	// layered views (3D, arrays, cube maps) attach layer 0 of mip 0, the view itself selects the slice
	static void AttachView(GLenum attachment, LatteTextureViewGL* view, bool allowLayered)
	{
		switch (view->dim)
		{
		case Latte::E_DIM::DIM_2D:
			glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, view->glTexId, 0);
			break;
		case Latte::E_DIM::DIM_2D_ARRAY:
			glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, view->glTexId, 0, 0);
			break;
		case Latte::E_DIM::DIM_3D:
		case Latte::E_DIM::DIM_CUBEMAP:
			if (allowLayered)
			{
				glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, view->glTexId, 0, 0);
				break;
			}
			[[fallthrough]];
		default:
			cemu_assert_suspicious();
			break;
		}
	}

	static void AttachColor(sint32 slot, LatteTextureViewGL* view)
	{
		const GLenum attachment = GL_COLOR_ATTACHMENT0 + slot;
		if (view)
			AttachView(attachment, view, true);
		else
			glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
	}

	static void AttachDepthStencil(LatteTextureViewGL* view, bool hasStencil)
	{
		if (!view)
		{
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
			return;
		}
		AttachView(GL_DEPTH_ATTACHMENT, view, false);
		if (hasStencil)
			AttachView(GL_STENCIL_ATTACHMENT, view, false);
		else
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
	}

	// draw buffer i always maps to colour attachment i, unused slots in between are GL_NONE
	void SelectDrawBuffers()
	{
		GLenum drawBuffers[8];
		GLsizei count = 0;
		for (sint32 slot = 0; slot < 8; slot++)
		{
			drawBuffers[slot] = colorBuffer[slot].texture ? (GLenum)(GL_COLOR_ATTACHMENT0 + slot) : (GLenum)GL_NONE;
			if (colorBuffer[slot].texture)
				count = slot + 1;
		}
		if (count == 0)
			glDrawBuffer(GL_NONE);
		else
			glDrawBuffers(count, drawBuffers);
	}
};
