// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Render2D.cpp

#include "Render2D.h"

#include <string>
#include "Shaders2D.h"

CRender2D::CRender2D(const Util::Config::Node &config)
  : m_config(config)
{
  glGenVertexArrays(1, &m_vao);   // empty: the vertex shader makes the corners
  glGenTextures(2, m_textures);
}

CRender2D::~CRender2D(void)
{
  if (m_vao)
    glDeleteVertexArrays(1, &m_vao);
  m_vao = 0;
  glDeleteTextures(2, m_textures);
  m_textures[0] = m_textures[1] = 0;
}

Result CRender2D::Init(unsigned xOffset, unsigned yOffset, unsigned xRes, unsigned yRes, unsigned totalXRes, unsigned totalYRes, unsigned aaTarget, UpscaleMode upscaleMode)
{
  m_xOffset = xOffset;
  m_yOffset = yOffset;
  m_xRes = xRes;
  m_yRes = yRes;
  m_totalXRes = totalXRes;
  m_totalYRes = totalYRes;
  m_aaTarget = aaTarget;
  // The layers sit 2 Model 3 lines off the 3D scene; move them by that
  // many (rounded) window pixels.
  m_shift = unsigned(double(yRes) / 384.0 * 2.0 + 0.5);
  const bool native = xRes == 496 && yRes == 384;
  m_upscaleMode = native ? UpscaleMode::Nearest : upscaleMode;

  const std::string fragment = std::string(s_fragmentShaderHeader) + "#define UPSCALEMODE " + std::to_string(int(m_upscaleMode)) + "\n" + s_fragmentShader;
  m_shader.LoadShaders(s_vertexShader, fragment.c_str());
  m_shader.GetUniformLocationMap("tex1");
  m_shader.EnableShader();
  glUniform1i(m_shader.uniformLocMap["tex1"], 0);
  m_shader.DisableShader();

  const GLint filter = (m_upscaleMode == UpscaleMode::Nearest) ? GL_NEAREST : GL_LINEAR;
  for (GLuint t : m_textures)
  {
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 496, 384, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }
  return Result::OKAY;
}

void CRender2D::AttachDrawBuffers(std::shared_ptr<TileGenBuffer> bottom, std::shared_ptr<TileGenBuffer> top)
{
  m_images[0] = bottom;
  m_images[1] = top;
}

void CRender2D::BeginFrame(void)
{
}

void CRender2D::PreRenderFrame(void)
{
  // Image lines are TileGenBuffer::Width pixels long; the first 496 show.
  glPixelStorei(GL_UNPACK_ROW_LENGTH, TileGenBuffer::Width);
  for (int i = 0; i < 2; i++)
  {
    if (!m_images[i])
      continue;
    glBindTexture(GL_TEXTURE_2D, m_textures[i]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 496, 384, GL_RGBA, GL_UNSIGNED_BYTE, m_images[i]->data);
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void CRender2D::RenderFrameBottom(void)
{
  PrepareLayer(true);
  DrawLayer(m_textures[0]);
}

void CRender2D::RenderFrameTop(void)
{
  PrepareLayer(false);
  DrawLayer(m_textures[1]);
}

void CRender2D::EndFrame(void)
{
}

void CRender2D::UseTarget(bool on) const
{
  if (m_aaTarget)
    glBindFramebuffer(GL_FRAMEBUFFER, on ? m_aaTarget : 0);
}

// Blending state and viewport for a layer. The bottom layer starts the
// frame: the whole window (borders included) is cleared first.
void CRender2D::PrepareLayer(bool bottom)
{
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_DEPTH_TEST);

  if (bottom)
  {
    UseTarget(true);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glViewport(0, 0, m_totalXRes, m_totalYRes);
    glDisable(GL_SCISSOR_TEST);   // the scissor box would keep the borders from clearing
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    UseTarget(false);
  }

  // A wide background keeps whatever viewport is set; otherwise the layer
  // covers the Model 3 screen area at its own aspect ratio.
  const bool wideBackground = bottom && m_config["WideBackground"].ValueAs<bool>();
  if (!wideBackground)
    glViewport(GLint(m_xOffset - m_shift), GLint(m_yOffset + m_shift), GLsizei(m_xRes), GLsizei(m_yRes));
}

void CRender2D::DrawLayer(GLuint texture)
{
  UseTarget(true);
  m_shader.EnableShader();
  glEnable(GL_BLEND);
  glBindVertexArray(m_vao);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glDisable(GL_BLEND);
  m_shader.DisableShader();
  UseTarget(false);
}

void CRender2D::WriteVRAM(unsigned addr, uint32_t data)
{
  (void)addr;
  (void)data;
}

void CRender2D::AttachRegisters(const uint32_t *regPtr)
{
  (void)regPtr;
}

void CRender2D::AttachPalette(const uint32_t *palPtr[2])
{
  (void)palPtr;
}

void CRender2D::AttachVRAM(const uint8_t *vramPtr)
{
  (void)vramPtr;
}
