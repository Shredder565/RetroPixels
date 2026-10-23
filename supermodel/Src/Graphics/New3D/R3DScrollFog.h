// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DScrollFog.h
//
// Fills the current viewport with fog colour (with the spotlight-on-fog
// glow), written straight into the 3D frame buffer. Its alpha decides how
// much of the 2D background shows through.

#ifndef INCLUDED_NEW3D_R3DSCROLLFOG_H
#define INCLUDED_NEW3D_R3DSCROLLFOG_H

#include <GL/glew.h>

namespace New3D {

class R3DScrollFog
{
public:
  R3DScrollFog();
  ~R3DScrollFog();

  // rgba: fog colour and coverage. attenuation: spotlight-on-fog strength,
  // ambient: fog colour scale, spotRGB / spotEllipse: the spotlight.
  void DrawScrollFog(float rgba[4], float attenuation, float ambient, float spotRGB[3], float spotEllipse[4]);

private:
  GLuint m_program = 0;
  GLuint m_vs = 0;
  GLuint m_fs = 0;
  GLint m_uColour = -1;
  GLint m_uAttenuation = -1;
  GLint m_uAmbient = -1;
  GLint m_uSpotColour = -1;
  GLint m_uSpotEllipse = -1;
  GLuint m_vao = 0;
};

} // New3D

#endif  // INCLUDED_NEW3D_R3DSCROLLFOG_H
