// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/SWOGLWindow.h"

#include <memory>
#include <string>

#include "Common/GL/GLContext.h"
#include "Common/GL/GLUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Software/SWTexture.h"

namespace
{
// A strip of four vertices generated from gl_VertexID covers the window; the texture is sampled
// with t flipped so the image's first row lands at the top.
constexpr const char* QUAD_VERTEX_SHADER =
    "out vec2 TexCoord;\n"
    "void main() {\n"
    "\tvec2 rawpos = vec2(gl_VertexID & 1, (gl_VertexID & 2) >> 1);\n"
    "\tgl_Position = vec4(rawpos * 2.0 - 1.0, 0.0, 1.0);\n"
    "\tTexCoord = vec2(rawpos.x, -rawpos.y);\n"
    "}\n";

constexpr const char* TEXTURE_FRAGMENT_SHADER = "in vec2 TexCoord;\n"
                                                "out vec4 ColorOut;\n"
                                                "uniform sampler2D samp;\n"
                                                "void main() {\n"
                                                "\tColorOut = texture(samp, TexCoord);\n"
                                                "}\n";

constexpr const char* GLES_PREAMBLE = "#version 300 es\n"
                                      "precision highp float;\n";
constexpr const char* DESKTOP_GL_PREAMBLE = "#version 140\n";

// The texture unit used for presentation, chosen to stay clear of anything else.
constexpr GLenum PRESENT_TEXTURE_UNIT = GL_TEXTURE9;
}  // namespace

SWOGLWindow::SWOGLWindow() = default;
SWOGLWindow::~SWOGLWindow() = default;

std::unique_ptr<SWOGLWindow> SWOGLWindow::Create(const WindowSystemInfo& wsi)
{
  std::unique_ptr<SWOGLWindow> window(new SWOGLWindow());
  if (window->Initialize(wsi))
    return window;

  PanicAlertFmt("Failed to create OpenGL window");
  return nullptr;
}

bool SWOGLWindow::IsHeadless() const
{
  return m_gl_context->IsHeadless();
}

bool SWOGLWindow::Initialize(const WindowSystemInfo& wsi)
{
  m_gl_context = GLContext::Create(wsi);
  if (!m_gl_context)
    return false;

  if (!GLExtensions::Init(m_gl_context.get()))
  {
    ERROR_LOG_FMT(VIDEO, "GLExtensions::Init failed!Does your video card support OpenGL 2.0?");
    return false;
  }
  const u32 gl_version = GLExtensions::Version();
  if (gl_version < 310)
  {
    ERROR_LOG_FMT(VIDEO, "OpenGL Version {} detected, but at least 3.1 is required.", gl_version);
    return false;
  }

  const std::string preamble = m_gl_context->IsGLES() ? GLES_PREAMBLE : DESKTOP_GL_PREAMBLE;
  m_image_program =
      GLUtil::CompileProgram(preamble + QUAD_VERTEX_SHADER, preamble + TEXTURE_FRAGMENT_SHADER);
  glUseProgram(m_image_program);
  glUniform1i(glGetUniformLocation(m_image_program, "samp"), 0);

  // Frames are shown pixel for pixel, without filtering.
  glGenTextures(1, &m_image_texture);
  glBindTexture(GL_TEXTURE_2D, m_image_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

  // Core profiles refuse to draw without a vertex array object bound, even an empty one.
  glGenVertexArrays(1, &m_image_vao);
  return true;
}

void SWOGLWindow::ShowImage(const AbstractTexture* image, const MathUtil::Rectangle<int>&)
{
  // The XFB region is not applied: the whole image is shown.
  const auto* frame = static_cast<const SW::SWTexture*>(image);
  const TextureConfig& config = frame->GetConfig();

  // Refreshes the window position and backbuffer size.
  m_gl_context->Update();
  glViewport(0, 0, static_cast<GLsizei>(m_gl_context->GetBackBufferWidth()),
             static_cast<GLsizei>(m_gl_context->GetBackBufferHeight()));

  glActiveTexture(PRESENT_TEXTURE_UNIT);
  glBindTexture(GL_TEXTURE_2D, m_image_texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(config.width));
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(config.width),
               static_cast<GLsizei>(config.height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
               frame->GetData(0, 0));

  glUseProgram(m_image_program);
  glBindVertexArray(m_image_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  m_gl_context->Swap();
}
