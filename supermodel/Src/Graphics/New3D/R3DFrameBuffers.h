// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DFrameBuffers.h
//
// The 3D scene is drawn into an off-screen frame buffer with three colour
// targets: opaque pixels, and the two Real3D translucency layers. Draw()
// composites them over whatever is in the bound frame buffer (the 2D layer
// under the 3D scene).

#ifndef INCLUDED_NEW3D_R3DFRAMEBUFFERS_H
#define INCLUDED_NEW3D_R3DFRAMEBUFFERS_H

#include <GL/glew.h>

#include "Types.h"
#include "GLSLShader.h"
#include "Model.h"

#ifdef SUPERMODEL_GLES
#include <cstdint>
#endif

namespace New3D {

class R3DFrameBuffers
{
public:
  R3DFrameBuffers();
  ~R3DFrameBuffers();

  Result CreateFBO(int width, int height);
  void DestroyFBO();

  // colour: draw to all three targets; trans1/trans2: only that layer;
  // none: back to the default frame buffer.
  void SetFBO(Layer layer);
  void BindTexture(Layer layer);

  // Save / restore depth and stencil, so both translucency layers are
  // tested against the opaque scene only.
  void StoreDepth();
  void RestoreDepth();

  void Draw();

#ifdef SUPERMODEL_GLES
  // OpenGL ES can't glReadPixels depth or stencil, so there the depth and
  // stencil buffer is a texture and this reads one pixel of it (window
  // coordinates) through a small draw: the depth as stored (32-bit float)
  // and the 8 stencil bits. Off the buffer both are 0, as glReadPixels
  // leaves them on desktop OpenGL. All GL state is put back.
  void ReadDepthStencil(int x, int y, float *depth, uint32_t *stencil);
#endif

private:
  GLuint m_fbo = 0;
  GLuint m_depth = 0;
  GLuint m_colour[3] = { 0, 0, 0 };
  GLuint m_saveFbo = 0;
  GLuint m_saveDepth = 0;
  Layer m_current = Layer::none;
  int m_width = 0;
  int m_height = 0;

  GLSLShader m_copyShader;
  GLSLShader m_blendShader;
  GLuint m_vao = 0;

#ifdef SUPERMODEL_GLES
  GLSLShader m_probeShader;     // writes the depth or stencil value it is asked for
  GLuint m_probeFbo = 0;        // 2x1 RGBA32UI: depth bits, stencil
  GLuint m_probeTarget = 0;
#endif
};

} // New3D

#endif  // INCLUDED_NEW3D_R3DFRAMEBUFFERS_H
