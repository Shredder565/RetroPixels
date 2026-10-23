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
};

} // New3D

#endif  // INCLUDED_NEW3D_R3DFRAMEBUFFERS_H
