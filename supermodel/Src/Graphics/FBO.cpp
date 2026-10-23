// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// FBO.cpp

#include "FBO.h"

bool FBO::Create(int width, int height)
{
  glGenTextures(1, &m_colour);
  glBindTexture(GL_TEXTURE_2D, m_colour);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colour, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return complete;
}

void FBO::Destroy()
{
  if (m_fbo)
  {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &m_fbo);
    m_fbo = 0;
  }
  if (m_colour)
  {
    glDeleteTextures(1, &m_colour);
    m_colour = 0;
  }
}

void FBO::BindTexture()
{
  glBindTexture(GL_TEXTURE_2D, m_colour);
}

void FBO::Set()
{
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
}

void FBO::Disable()
{
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

GLuint FBO::GetFBOID()
{
  return m_fbo;
}

GLuint FBO::GetTextureID()
{
  return m_colour;
}
