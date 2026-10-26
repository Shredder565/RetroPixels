// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DFrameBuffers.cpp

#include "R3DFrameBuffers.h"

#ifdef SUPERMODEL_GLES
#include <cstring>
#endif

namespace New3D {

namespace
{
  // Full-screen quad from gl_VertexID; no vertex buffers needed.
  const char *kScreenVS = R"glsl(
#version 410 core
void main(void)
{
  vec2 corner = vec2(float((gl_VertexID & 2) >> 1), float(gl_VertexID & 1));
  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

  const char *kCopyFS = R"glsl(
#version 410 core
uniform sampler2D layer0;
out vec4 result;
void main(void)
{
  result = texelFetch(layer0, ivec2(gl_FragCoord.xy), 0);
}
)glsl";

  // The two translucency layers. Where both are present they are mixed
  // into an opaque pixel, otherwise whichever one is present is used.
  const char *kBlendFS = R"glsl(
#version 410 core
uniform sampler2D layer1;
uniform sampler2D layer2;
out vec4 result;
void main(void)
{
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 a = texelFetch(layer1, p, 0);
  vec4 b = texelFetch(layer2, p, 0);
  if (a.a * b.a > 0.0)
    result = vec4(mix(a.rgb, b.rgb, (b.a + (1.0 - a.a)) / 2.0), 1.0);
  else if (a.a > 0.0)
    result = a;
  else
    result = b;
}
)glsl";

  GLuint MakeColourTarget(int width, int height)
  {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return id;
  }

#ifdef SUPERMODEL_GLES
  // OpenGL ES: one pixel of the depth/stencil texture, the depth (as its
  // bits) or the stencil value, whichever uWhat asks for. The texture's
  // DEPTH_STENCIL_TEXTURE_MODE is set to match before each draw.
  const char *kProbeFS = R"glsl(
#version 410 core
uniform sampler2D uDepth;
uniform usampler2D uStencil;
uniform ivec2 uPixel;
uniform int uWhat;
layout(location = 0) out uvec4 result;
void main(void)
{
  if (uWhat == 0)
    result = uvec4(floatBitsToUint(texelFetch(uDepth, uPixel, 0).r), 0u, 0u, 0u);
  else
    result = uvec4(texelFetch(uStencil, uPixel, 0).r, 0u, 0u, 0u);
}
)glsl";

  // Texture units the probe uses, well away from the renderers' own.
  const GLenum kDepthUnit = 14;
  const GLenum kStencilUnit = 15;

  // The depth/stencil buffer as a texture, so it can be read (ES only).
  GLuint MakeDepthStencilTexture(int width, int height)
  {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH32F_STENCIL8, width, height);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, id, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    return id;
  }
#endif

  GLuint MakeDepthStencil(int width, int height)
  {
    GLuint id = 0;
    glGenRenderbuffers(1, &id);
    glBindRenderbuffer(GL_RENDERBUFFER, id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, id);
    return id;
  }
}

R3DFrameBuffers::R3DFrameBuffers()
{
  m_blendShader.LoadShaders(kScreenVS, kBlendFS);
  m_blendShader.uniformLoc[0] = m_blendShader.GetUniformLocation("layer1");
  m_blendShader.uniformLoc[1] = m_blendShader.GetUniformLocation("layer2");
  m_copyShader.LoadShaders(kScreenVS, kCopyFS);
  m_copyShader.uniformLoc[0] = m_copyShader.GetUniformLocation("layer0");
  glGenVertexArrays(1, &m_vao);

#ifdef SUPERMODEL_GLES
  m_probeShader.LoadShaders(kScreenVS, kProbeFS);
  m_probeShader.uniformLoc[0] = m_probeShader.GetUniformLocation("uDepth");
  m_probeShader.uniformLoc[1] = m_probeShader.GetUniformLocation("uStencil");
  m_probeShader.uniformLoc[2] = m_probeShader.GetUniformLocation("uPixel");
  m_probeShader.uniformLoc[3] = m_probeShader.GetUniformLocation("uWhat");
  m_probeShader.EnableShader();
  glUniform1i(m_probeShader.uniformLoc[0], GLint(kDepthUnit));
  glUniform1i(m_probeShader.uniformLoc[1], GLint(kStencilUnit));
  m_probeShader.DisableShader();

  glGenTextures(1, &m_probeTarget);
  glBindTexture(GL_TEXTURE_2D, m_probeTarget);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32UI, 2, 1);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &m_probeFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_probeFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_probeTarget, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
}

R3DFrameBuffers::~R3DFrameBuffers()
{
  DestroyFBO();
  m_blendShader.UnloadShaders();
  m_copyShader.UnloadShaders();
  if (m_vao)
    glDeleteVertexArrays(1, &m_vao);
  m_vao = 0;
#ifdef SUPERMODEL_GLES
  m_probeShader.UnloadShaders();
  if (m_probeFbo)
    glDeleteFramebuffers(1, &m_probeFbo);
  if (m_probeTarget)
    glDeleteTextures(1, &m_probeTarget);
  m_probeFbo = m_probeTarget = 0;
#endif
}

Result R3DFrameBuffers::CreateFBO(int width, int height)
{
  m_width = width;
  m_height = height;

  for (GLuint &t : m_colour)
    t = MakeColourTarget(width, height);

  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  for (int i = 0; i < 3; i++)
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, m_colour[i], 0);
#ifdef SUPERMODEL_GLES
  m_depth = MakeDepthStencilTexture(width, height);
#else
  m_depth = MakeDepthStencil(width, height);
#endif
  const bool mainComplete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

  glGenFramebuffers(1, &m_saveFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_saveFbo);
  m_saveDepth = MakeDepthStencil(width, height);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return mainComplete ? Result::OKAY : Result::FAIL;
}

void R3DFrameBuffers::DestroyFBO()
{
  if (m_fbo)
  {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#ifdef SUPERMODEL_GLES
    glDeleteTextures(1, &m_depth);
#else
    glDeleteRenderbuffers(1, &m_depth);
#endif
    glDeleteFramebuffers(1, &m_fbo);
  }
  if (m_saveFbo)
  {
    glDeleteRenderbuffers(1, &m_saveDepth);
    glDeleteFramebuffers(1, &m_saveFbo);
  }
  for (GLuint &t : m_colour)
  {
    if (t)
      glDeleteTextures(1, &t);
    t = 0;
  }
  m_fbo = m_depth = m_saveFbo = m_saveDepth = 0;
  m_width = m_height = 0;
}

void R3DFrameBuffers::BindTexture(Layer layer)
{
  glBindTexture(GL_TEXTURE_2D, m_colour[int(layer)]);
}

void R3DFrameBuffers::SetFBO(Layer layer)
{
  if (layer == m_current)
    return;

  if (layer == Layer::none)
  {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
  }
  else if (layer == Layer::colour || layer == Layer::trans1 || layer == Layer::trans2)
  {
    GLenum targets[3] = { GL_NONE, GL_NONE, GL_NONE };
    for (int i = 0; i < 3; i++)
    {
      if (layer == Layer::colour || int(layer) == i)
        targets[i] = GL_COLOR_ATTACHMENT0 + i;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glDrawBuffers(3, targets);
  }
  m_current = layer;
}

void R3DFrameBuffers::StoreDepth()
{
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_saveFbo);
  glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
}

void R3DFrameBuffers::RestoreDepth()
{
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_saveFbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
  glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
}

void R3DFrameBuffers::Draw()
{
  glViewport(0, 0, m_width, m_height);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_BLEND);

  for (int i = 0; i < 3; i++)
  {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, m_colour[i]);
  }
  glActiveTexture(GL_TEXTURE0);
  glBindVertexArray(m_vao);

  m_copyShader.EnableShader();
  glUniform1i(m_copyShader.uniformLoc[0], 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  m_blendShader.EnableShader();
  glUniform1i(m_blendShader.uniformLoc[0], 1);
  glUniform1i(m_blendShader.uniformLoc[1], 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  m_blendShader.DisableShader();

  glDisable(GL_BLEND);
  glBindVertexArray(0);
}

#ifdef SUPERMODEL_GLES
void R3DFrameBuffers::ReadDepthStencil(int x, int y, float *depth, uint32_t *stencil)
{
  *depth = 0.0f;
  *stencil = 0;
  if (!m_fbo || x < 0 || y < 0 || x >= m_width || y >= m_height)
    return;

  // What the draw below changes, to be put back.
  GLint drawFbo = 0, readFbo = 0, program = 0, vao = 0, active = 0, packBuffer = 0;
  GLint viewport[4] = { 0, 0, 0, 0 };
  GLint unitTex[2] = { 0, 0 };
  GLboolean mask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
  glGetIntegerv(GL_VIEWPORT, viewport);
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  const GLenum caps[] = { GL_DEPTH_TEST, GL_STENCIL_TEST, GL_BLEND, GL_SCISSOR_TEST, GL_CULL_FACE };
  GLboolean enabled[5];
  for (int i = 0; i < 5; i++)
  {
    enabled[i] = glIsEnabled(caps[i]);
    glDisable(caps[i]);
  }
  glActiveTexture(GL_TEXTURE0 + kDepthUnit);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &unitTex[0]);
  glActiveTexture(GL_TEXTURE0 + kStencilUnit);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &unitTex[1]);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  glBindFramebuffer(GL_FRAMEBUFFER, m_probeFbo);
  const GLenum target = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &target);
  glBindVertexArray(m_vao);
  m_probeShader.EnableShader();
  glUniform2i(m_probeShader.uniformLoc[2], x, y);

  // Depth into the first pixel, then stencil into the second: the texture
  // is sampled as one or the other, never both in one draw.
  for (int what = 0; what < 2; what++)
  {
    glActiveTexture(GL_TEXTURE0 + kDepthUnit);
    glBindTexture(GL_TEXTURE_2D, what == 0 ? m_depth : 0);
    glActiveTexture(GL_TEXTURE0 + kStencilUnit);
    glBindTexture(GL_TEXTURE_2D, what == 1 ? m_depth : 0);
    glActiveTexture(GL_TEXTURE0 + (what == 0 ? kDepthUnit : kStencilUnit));
    glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, what == 0 ? GL_DEPTH_COMPONENT : GL_STENCIL_INDEX);
    glUniform1i(m_probeShader.uniformLoc[3], what);
    glViewport(what, 0, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);

  GLuint pixels[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glReadPixels(0, 0, 2, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, pixels);
  std::memcpy(depth, &pixels[0], sizeof(float));
  *stencil = pixels[4] & 0xFF;

  // Put everything back.
  glActiveTexture(GL_TEXTURE0 + kDepthUnit);
  glBindTexture(GL_TEXTURE_2D, GLuint(unitTex[0]));
  glActiveTexture(GL_TEXTURE0 + kStencilUnit);
  glBindTexture(GL_TEXTURE_2D, GLuint(unitTex[1]));
  glActiveTexture(GLenum(active));
  glBindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(packBuffer));
  glUseProgram(GLuint(program));
  glBindVertexArray(GLuint(vao));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFbo));
  glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(readFbo));
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  glColorMask(mask[0], mask[1], mask[2], mask[3]);
  for (int i = 0; i < 5; i++)
  {
    if (enabled[i])
      glEnable(caps[i]);
  }
}
#endif

} // New3D
