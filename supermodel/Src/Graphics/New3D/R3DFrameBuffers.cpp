// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DFrameBuffers.cpp

#include "R3DFrameBuffers.h"

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
}

R3DFrameBuffers::~R3DFrameBuffers()
{
  DestroyFBO();
  m_blendShader.UnloadShaders();
  m_copyShader.UnloadShaders();
  if (m_vao)
    glDeleteVertexArrays(1, &m_vao);
  m_vao = 0;
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
  m_depth = MakeDepthStencil(width, height);
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
    glDeleteRenderbuffers(1, &m_depth);
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

} // New3D
