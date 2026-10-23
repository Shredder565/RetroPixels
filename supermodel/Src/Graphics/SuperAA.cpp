// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// SuperAA.cpp

#include "SuperAA.h"

#include <string>

namespace
{
  // A full-window triangle strip made from gl_VertexID (no vertex data).
  const char kVertexShader[] = R"glsl(
#version 410 core

void main(void)
{
  vec2 corner = vec2(float((gl_VertexID & 2) >> 1), float(gl_VertexID & 1));
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

  // Box filter over each AA x AA block of the target. CRT_COLOURS picks a
  // conversion from an arcade monitor's colour space to sRGB: decode its
  // gamma (or the sRGB curve), apply a 3x3 gamut matrix (D93 white point
  // adapted to D65; values from public colour-matrix tables), encode sRGB.
  const char kFragmentShader[] = R"glsl(
uniform sampler2D uFrame;
out vec4 fragColour;

#if CRT_COLOURS == 1
  // Japanese arcade CRT (ARI D93), gamma 2.5
  const float kGamma = 2.5;
  const mat3 kGamut = mat3(1.3272128334714093, -0.3802108879412366, -0.1003607696463202,
                           -0.0241848600268417, 0.9544506228550125, 0.0807358585208939,
                           -0.0239585810555497, -0.0409736005706461, 1.4076563728858242);
#elif CRT_COLOURS == 2
  // Sony PVM-20M2U (D93), gamma 2.25
  const float kGamma = 2.25;
  const mat3 kGamut = mat3(0.9241392201737613, -0.0690701363506526, -0.0084279079392561,
                           0.0231962420140721, 0.9729221778325590, 0.0148832015024337,
                           -0.0054875731998806, 0.0098220960740544, 1.3383896683854550);
#elif CRT_COLOURS == 3
  // BT.601 525-line, D93 (Japan), sRGB curve
  const float kGamma = -1.0;
  const mat3 kGamut = mat3(0.7822490684754086, 0.0506168576073398, 0.0137752498011040,
                           0.0147968947158740, 0.9741745313046067, 0.0220301953285839,
                           -0.0013501205469208, -0.0044076726925543, 1.3484819844991036);
#elif CRT_COLOURS == 4
  // BT.601 525-line (US), sRGB curve
  const float kGamma = -1.0;
  const mat3 kGamut = mat3(0.9395420637732393, 0.0501813568598678, 0.0102765793668928,
                           0.0177722231435608, 0.9657928624969044, 0.0164349143595347,
                           -0.0016215999431855, -0.0043697496597356, 1.0059913496029214);
#elif CRT_COLOURS == 5
  // BT.601 625-line (Europe, Australia), sRGB curve
  const float kGamma = -1.0;
  const mat3 kGamut = mat3(1.0440432087628346, -0.0440432087628348, 0.0000000000000001,
                           0.0000000000000000, 1.0000000000000002, 0.0000000000000000,
                           0.0000000000000000, 0.0117933782840052, 0.9882066217159947);
#endif

float ToSRGB(float c)
{
  return (c <= 0.0031308) ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

float FromSRGB(float c)
{
  return (c <= 0.04045) ? c * (1.0 / 12.92) : pow(c * (1.0 / 1.055) + (0.055 / 1.055), 2.4);
}

void main(void)
{
  ivec2 base = ivec2(gl_FragCoord.xy) * AA;
  vec3 sum = vec3(0.0);
  for (int x = 0; x < AA; x++)
    for (int y = 0; y < AA; y++)
      sum += texelFetch(uFrame, base + ivec2(x, y), 0).rgb;
  vec3 colour = sum * (1.0 / float(AA * AA));

#if CRT_COLOURS != 0
  if (kGamma == -1.0)
    colour = vec3(FromSRGB(colour.r), FromSRGB(colour.g), FromSRGB(colour.b));
  else
    colour = pow(colour, vec3(kGamma));
  colour = colour * kGamut;
  colour = vec3(ToSRGB(colour.r), ToSRGB(colour.g), ToSRGB(colour.b));
#endif

  fragColour = vec4(colour, 1.0);
}
)glsl";
}

SuperAA::SuperAA(int aaValue, CRTcolor CRTcolors)
  : m_aa(aaValue),
    m_crtcolors(CRTcolors)
{
  if (!Active())
    return;

  const std::string fragment = "#version 410 core\n#define AA " + std::to_string(m_aa) +
    "\n#define CRT_COLOURS " + std::to_string(int(m_crtcolors)) + "\n" + kFragmentShader;
  m_shader.LoadShaders(kVertexShader, fragment.c_str());
  m_shader.GetUniformLocationMap("uFrame");
  m_shader.EnableShader();
  glUniform1i(m_shader.uniformLocMap["uFrame"], 0);
  m_shader.DisableShader();

  glGenVertexArrays(1, &m_vao);   // empty: the vertex shader makes the corners
}

SuperAA::~SuperAA()
{
  m_shader.UnloadShaders();
  m_fbo.Destroy();
  if (m_vao)
    glDeleteVertexArrays(1, &m_vao);
  m_vao = 0;
}

bool SuperAA::Active() const
{
  return m_aa > 1 || m_crtcolors != CRTcolor::None;
}

void SuperAA::Init(int width, int height)
{
  if (!Active())
    return;
  m_fbo.Destroy();
  m_fbo.Create(width * m_aa, height * m_aa);
  m_width = width;
  m_height = height;
}

void SuperAA::Draw()
{
  if (!Active())
    return;

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);

  glBindTexture(GL_TEXTURE_2D, m_fbo.GetTextureID());
  glBindVertexArray(m_vao);
  glViewport(0, 0, m_width, m_height);
  m_shader.EnableShader();
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  m_shader.DisableShader();
  glBindVertexArray(0);
}

GLuint SuperAA::GetTargetID()
{
  return m_fbo.GetFBOID();    // 0 when inactive: draw straight to the window
}
