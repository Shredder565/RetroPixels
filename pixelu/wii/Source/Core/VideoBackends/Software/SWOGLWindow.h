// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"

class AbstractTexture;
class GLContext;
struct WindowSystemInfo;

// The window the software renderer presents into: an OpenGL context that draws each finished
// frame as one full-window textured quad.
class SWOGLWindow
{
public:
  ~SWOGLWindow();

  GLContext* GetContext() const { return m_gl_context.get(); }
  bool IsHeadless() const;

  // Uploads the image and presents it straight away.
  void ShowImage(const AbstractTexture* image, const MathUtil::Rectangle<int>& xfb_region);

  // Returns null (after telling the user) when no usable OpenGL 3.1 context can be made.
  static std::unique_ptr<SWOGLWindow> Create(const WindowSystemInfo& wsi);

private:
  SWOGLWindow();

  bool Initialize(const WindowSystemInfo& wsi);

  u32 m_image_program = 0;
  u32 m_image_texture = 0;
  u32 m_image_vao = 0;

  std::unique_ptr<GLContext> m_gl_context;
};
