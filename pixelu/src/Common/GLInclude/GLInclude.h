// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Single include for the OpenGL renderer: pulls in the Khronos extension
// headers, declares the runtime-loaded function pointers listed in
// glFunctions.h (namespace CemuGL, kept for the existing callers) and offers
// texture helpers that use direct state access when the driver has it and
// otherwise bind the texture briefly and restore the previous binding.
#pragma once

#include "glext.h"

#if BOOST_OS_WINDOWS > 0
#include "wglext.h"
#endif

#if (BOOST_OS_LINUX || BOOST_OS_BSD) > 0
// glxext.h expects the Xlib / GLX base types, which we do not include.
#define Bool int
#define Status int
#define True 1
#define False 0
typedef XID GLXContextID;
typedef XID GLXPixmap;
typedef XID GLXDrawable;
typedef XID GLXPbuffer;
typedef XID GLXWindow;
typedef XID GLXFBConfigID;
typedef struct __GLXcontextRec* GLXContext;
typedef struct __GLXFBConfigRec* GLXFBConfig;

#define EGL_EGL_PROTOTYPES 0 // only the PFN typedefs; functions are loaded at runtime
#include "egl.h"
#undef EGL_EGL_PROTOTYPES
#include "glxext.h"

#undef Bool
#undef Status
#undef True
#undef False
#endif

namespace CemuGL
{
#define GLFUNC(type, name) extern type name;
#define EGLFUNC(type, name) extern type name;
#include "glFunctions.h"
#undef GLFUNC
#undef EGLFUNC

	// Binding query enum for a texture target.
	static GLenum GetGLBindingFromTextureTarget(GLenum target)
	{
		switch (target)
		{
		case GL_TEXTURE_1D: return GL_TEXTURE_BINDING_1D;
		case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
		case GL_TEXTURE_3D: return GL_TEXTURE_BINDING_3D;
		case GL_TEXTURE_2D_ARRAY: return GL_TEXTURE_BINDING_2D_ARRAY;
		case GL_TEXTURE_CUBE_MAP: return GL_TEXTURE_BINDING_CUBE_MAP;
		case GL_TEXTURE_CUBE_MAP_ARRAY: return GL_TEXTURE_BINDING_CUBE_MAP_ARRAY;
		}
		cemu_assert_unimplemented();
		return 0;
	}

	// Binds a texture for the lifetime of the object and puts back whatever
	// was bound to the same target before. Used when DSA is unavailable.
	struct GLScopedTextureBind
	{
		GLScopedTextureBind(GLenum target, GLuint texture) : m_target(target)
		{
			glGetIntegerv(GetGLBindingFromTextureTarget(target), &m_previous);
			glBindTexture(target, texture);
		}
		~GLScopedTextureBind()
		{
			glBindTexture(m_target, (GLuint)m_previous);
		}
		GLScopedTextureBind(const GLScopedTextureBind&) = delete;
		GLScopedTextureBind& operator=(const GLScopedTextureBind&) = delete;

	  private:
		GLenum m_target;
		GLint m_previous = 0;
	};

	static GLuint glCreateTextureWrapper(GLenum target)
	{
		GLuint texture = 0;
		if (glCreateTextures)
		{
			glCreateTextures(target, 1, &texture);
			return texture;
		}
		// a name from glGenTextures only becomes a texture object once bound
		glGenTextures(1, &texture);
		GLScopedTextureBind bind(target, texture);
		return texture;
	}

	static void glTextureStorage1DWrapper(GLenum target, GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width)
	{
		if (glTextureStorage1D)
			return glTextureStorage1D(texture, levels, internalformat, width);
		GLScopedTextureBind bind(target, texture);
		glTexStorage1D(target, levels, internalformat, width);
	}

	static void glTextureStorage2DWrapper(GLenum target, GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)
	{
		if (glTextureStorage2D)
			return glTextureStorage2D(texture, levels, internalformat, width, height);
		GLScopedTextureBind bind(target, texture);
		glTexStorage2D(target, levels, internalformat, width, height);
	}

	static void glTextureStorage3DWrapper(GLenum target, GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth)
	{
		if (glTextureStorage3D)
			return glTextureStorage3D(texture, levels, internalformat, width, height, depth);
		GLScopedTextureBind bind(target, texture);
		glTexStorage3D(target, levels, internalformat, width, height, depth);
	}

	static void glTextureSubImage1DWrapper(GLenum target, GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const void* pixels)
	{
		if (glTextureSubImage1D)
			return glTextureSubImage1D(texture, level, xoffset, width, format, type, pixels);
		GLScopedTextureBind bind(target, texture);
		glTexSubImage1D(target, level, xoffset, width, format, type, pixels);
	}

	static void glCompressedTextureSubImage1DWrapper(GLenum target, GLuint texture, GLint level, GLint xoffset, GLsizei width, GLenum format, GLsizei imageSize, const void* data)
	{
		if (glCompressedTextureSubImage1D)
			return glCompressedTextureSubImage1D(texture, level, xoffset, width, format, imageSize, data);
		GLScopedTextureBind bind(target, texture);
		glCompressedTexSubImage1D(target, level, xoffset, width, format, imageSize, data);
	}

	static void glTextureSubImage2DWrapper(GLenum target, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
	{
		if (glTextureSubImage2D)
			return glTextureSubImage2D(texture, level, xoffset, yoffset, width, height, format, type, pixels);
		GLScopedTextureBind bind(target, texture);
		glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
	}

	static void glCompressedTextureSubImage2DWrapper(GLenum target, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data)
	{
		if (glCompressedTextureSubImage2D)
			return glCompressedTextureSubImage2D(texture, level, xoffset, yoffset, width, height, format, imageSize, data);
		GLScopedTextureBind bind(target, texture);
		glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
	}

	static void glTextureSubImage3DWrapper(GLenum target, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
	{
		if (glTextureSubImage3D)
			return glTextureSubImage3D(texture, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
		GLScopedTextureBind bind(target, texture);
		glTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, pixels);
	}

	static void glCompressedTextureSubImage3DWrapper(GLenum target, GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLsizei imageSize, const void* data)
	{
		if (glCompressedTextureSubImage3D)
			return glCompressedTextureSubImage3D(texture, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, data);
		GLScopedTextureBind bind(target, texture);
		glCompressedTexSubImage3D(target, level, xoffset, yoffset, zoffset, width, height, depth, format, imageSize, data);
	}
} // namespace CemuGL

using namespace CemuGL;

// keep the Windows SDK's GL.h out: everything comes from the headers above
#define __gl_h_
#define __GL_H__
