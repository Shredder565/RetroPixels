// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DScrollFog.cpp

#include "R3DScrollFog.h"
#include "Graphics/Shader.h"

namespace New3D {

namespace
{
  const char *kFogVS = R"glsl(
#version 410 core
void main(void)
{
  vec2 corner = vec2(float((gl_VertexID & 2) >> 1), float(gl_VertexID & 1));
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

  const char *kFogFS = R"glsl(
#version 410 core
uniform vec4  fogRGBA;
uniform float fogAmbientScale;
uniform float spotStrength;
uniform vec3  spotRGB;
uniform vec4  spotShape;    // centre x/y and half size x/y, in window pixels

layout(location = 0) out vec4 outOpaque;
layout(location = 1) out vec4 outTrans1;
layout(location = 2) out vec4 outTrans2;

void main(void)
{
  // Spotlight on fog: brightest at the ellipse centre, gone at its edge.
  float r = length((gl_FragCoord.xy - spotShape.xy) / spotShape.zw);
  float glow = max(0.0, 1.0 - r * r);
  vec3 spot = mix(vec3(0.0), spotRGB * glow * fogRGBA.rgb, spotStrength);

  outOpaque = vec4(fogRGBA.rgb * fogAmbientScale + spot, fogRGBA.a);
  outTrans1 = vec4(0.0);
  outTrans2 = vec4(0.0);
}
)glsl";
}

R3DScrollFog::R3DScrollFog()
{
  LoadShaderProgram(&m_program, &m_vs, &m_fs, "", "", kFogVS, kFogFS);
  m_uColour = glGetUniformLocation(m_program, "fogRGBA");
  m_uAttenuation = glGetUniformLocation(m_program, "spotStrength");
  m_uAmbient = glGetUniformLocation(m_program, "fogAmbientScale");
  m_uSpotColour = glGetUniformLocation(m_program, "spotRGB");
  m_uSpotEllipse = glGetUniformLocation(m_program, "spotShape");
  glGenVertexArrays(1, &m_vao);
}

R3DScrollFog::~R3DScrollFog()
{
  if (m_program)
    DestroyShaderProgram(m_program, m_vs, m_fs);
  m_program = m_vs = m_fs = 0;
  if (m_vao)
    glDeleteVertexArrays(1, &m_vao);
  m_vao = 0;
}

void R3DScrollFog::DrawScrollFog(float rgba[4], float attenuation, float ambient, float spotRGB[3], float spotEllipse[4])
{
  glDepthMask(GL_FALSE);
  glDisable(GL_DEPTH_TEST);
  glBindVertexArray(m_vao);
  glUseProgram(m_program);
  glUniform4fv(m_uColour, 1, rgba);
  glUniform1f(m_uAttenuation, attenuation);
  glUniform1f(m_uAmbient, ambient);
  glUniform3fv(m_uSpotColour, 1, spotRGB);
  glUniform4fv(m_uSpotEllipse, 1, spotEllipse);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glUseProgram(0);
  glBindVertexArray(0);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
}

} // New3D
