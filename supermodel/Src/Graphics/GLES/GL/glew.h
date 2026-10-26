// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// OpenGL ES 3.2 in place of GLEW and desktop OpenGL, for the builds made with
// SUPERMODEL_GLES (Android, and the Linux GLES test build). This folder comes
// first on those builds' include path, so the renderers' own
// "#include <GL/glew.h>" lands here and their code stays as it is for the
// desktop builds, which keep GLEW and OpenGL 4.1.
//
// What the shared code uses that OpenGL ES doesn't have:
//   - glClearDepth (ES has glClearDepthf) and glDrawBuffer (ES has
//     glDrawBuffers): small inline stand-ins below;
//   - desktop GLSL ("#version 410 core"): every glShaderSource call goes
//     through GLES::ShaderSource, which makes the program "#version 320 es"
//     with highp precision (GLES.cpp);
//   - reading depth and stencil with glReadPixels (line of sight): New3D's
//     frame buffers read them through a draw on SUPERMODEL_GLES builds
//     (R3DFrameBuffers::ReadDepthStencil);
//   - GL_BGRA uploads: the crosshair converts its pictures to RGBA.
// The quad pipeline (QuadRendering, a geometry shader with noperspective
// inputs) and the legacy 3D engine are left out of these builds.

#ifndef INCLUDED_GLES_GL_GLEW_H
#define INCLUDED_GLES_GL_GLEW_H

#ifndef SUPERMODEL_GLES
#error "Graphics/GLES is only on the include path of SUPERMODEL_GLES builds"
#endif

#include <stddef.h>   // GLEW's header brings size_t and ptrdiff_t, and code relies on it
#include <GLES3/gl32.h>

namespace GLES
{
  // glShaderSource with the sources made OpenGL ES 3.20 shaders.
  void ShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths);
}

inline void glClearDepth(double depth)
{
  glClearDepthf(GLfloat(depth));
}

// The default frame buffer's back buffer, or one colour attachment.
inline void glDrawBuffer(GLenum buffer)
{
  glDrawBuffers(1, &buffer);
}

#define glShaderSource GLES::ShaderSource

#endif // INCLUDED_GLES_GL_GLEW_H
