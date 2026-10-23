// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// FBO.h
//
// An off-screen frame buffer with one RGBA8 colour texture (linear
// filtering, clamped edges), used as a render target that is later drawn
// from.

#ifndef INCLUDED_FBO_H
#define INCLUDED_FBO_H

#include <GL/glew.h>

class FBO
{
public:
  FBO() = default;

  bool Create(int width, int height);   // true if the frame buffer is complete
  void Destroy();
  void BindTexture();                   // binds the colour texture to GL_TEXTURE_2D
  void Set();                           // draw into this frame buffer
  void Disable();                       // back to the default frame buffer
  GLuint GetFBOID();
  GLuint GetTextureID();

private:
  GLuint m_fbo = 0;
  GLuint m_colour = 0;
};

#endif  // INCLUDED_FBO_H
