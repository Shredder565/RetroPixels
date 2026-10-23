// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// SuperAA.h
//
// Super-sampling and CRT colour output. With a factor N above 1 the frame
// is drawn at N times the window size into an off-screen target, and Draw()
// averages each NxN block into one window pixel. A CRT colour mode converts
// the colours from an arcade monitor's gamut and gamma to sRGB on the way.
// With N = 1 and no CRT colours it does nothing and the frame is drawn
// straight to the window.

#ifndef INCLUDED_SUPERAA_H
#define INCLUDED_SUPERAA_H

#include "Supermodel.h"
#include "FBO.h"
#include "New3D/GLSLShader.h"

class SuperAA
{
public:
  SuperAA(int aaValue, CRTcolor CRTcolors);
  ~SuperAA();     // needs the GL context current

  void Init(int width, int height);   // window size in pixels
  void Draw();                        // resolves the target into the window

  GLuint GetTargetID();               // frame buffer to draw the frame into (0: the window)

private:
  bool Active() const;

  FBO m_fbo;
  GLSLShader m_shader;
  const int m_aa;
  const CRTcolor m_crtcolors;
  GLuint m_vao = 0;
  int m_width = 0;
  int m_height = 0;
};

#endif  // INCLUDED_SUPERAA_H
