// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/GL/GLContext.h"

#include <memory>

#ifdef _WIN32
#include "Common/GL/GLInterface/WGL.h"
#endif
// PIXELU_GL_GLX_EGL: set by Common/CMakeLists.txt on Linux, where GLInterface/GLX.cpp, EGL.cpp
// and X11Window.cpp are built.
#ifdef PIXELU_GL_GLX_EGL
#include "Common/GL/GLInterface/EGL.h"
#include "Common/GL/GLInterface/GLX.h"
#endif

GLContext::~GLContext() = default;

// The base class stands for "no GL available": nothing succeeds.

bool GLContext::Initialize(const WindowSystemInfo&, bool, bool)
{
  return false;
}

bool GLContext::IsHeadless() const
{
  return true;
}

std::unique_ptr<GLContext> GLContext::CreateSharedContext()
{
  return {};
}

bool GLContext::MakeCurrent()
{
  return false;
}

bool GLContext::ClearCurrent()
{
  return false;
}

void GLContext::Update()
{
}

void GLContext::UpdateSurface(void*)
{
}

void GLContext::SetSurfaceSize(u32, u32)
{
}

void GLContext::Swap()
{
}

void GLContext::SwapInterval(int)
{
}

void* GLContext::GetFuncAddress(const std::string&)
{
  return nullptr;
}

namespace
{
#ifdef PIXELU_GL_GLX_EGL
enum class Interface
{
  None,
  GLX,
  EGL,
};

// On Linux a window system can have two implementations, tried in order: X11 uses GLX
// (desktop GL, what drivers support best) and EGL when GLX fails or OpenGL ES is preferred
// (GLX only does desktop GL here); Wayland and headless use EGL.
std::array<Interface, 2> InterfacesFor(WindowSystemType type, bool prefer_gles)
{
  switch (type)
  {
  case WindowSystemType::X11:
    if (prefer_gles)
      return {Interface::EGL, Interface::GLX};
    return {Interface::GLX, Interface::EGL};
  case WindowSystemType::Wayland:
  case WindowSystemType::Headless:
    return {Interface::EGL, Interface::None};
  default:
    return {Interface::None, Interface::None};
  }
}

std::unique_ptr<GLContext> NewContext(Interface which)
{
  switch (which)
  {
  case Interface::GLX:
    return std::make_unique<GLContextGLX>();
  case Interface::EGL:
    return std::make_unique<GLContextEGL>();
  default:
    return nullptr;
  }
}
#else
// Picks the context implementation for a window system, or nullptr when this build has none.
std::unique_ptr<GLContext> NewContextFor(WindowSystemType type)
{
#ifdef _WIN32
  if (type == WindowSystemType::Windows)
    return std::make_unique<GLContextWGL>();
#else
  (void)type;
#endif
  return nullptr;
}
#endif
}  // namespace

#ifdef PIXELU_GL_GLX_EGL
std::unique_ptr<GLContext> GLContext::Create(const WindowSystemInfo& wsi, bool stereo, bool core,
                                             bool prefer_gles)
{
  for (const Interface which : InterfacesFor(wsi.type, prefer_gles))
  {
    std::unique_ptr<GLContext> context = NewContext(which);
    if (context == nullptr)
      continue;
    if (prefer_gles)
      context->m_mode = Mode::OpenGLES;
    if (context->Initialize(wsi, stereo, core))
      return context;
  }
  return nullptr;
}
#else
std::unique_ptr<GLContext> GLContext::Create(const WindowSystemInfo& wsi, bool stereo, bool core,
                                             bool prefer_gles)
{
  std::unique_ptr<GLContext> context = NewContextFor(wsi.type);
  if (context == nullptr)
    return nullptr;

  // Asking for ES up front lets an implementation that can do both pick it (handy for testing
  // the ES paths on a desktop driver). An implementation that only does desktop GL overrides it.
  if (prefer_gles)
    context->m_mode = Mode::OpenGLES;

  if (!context->Initialize(wsi, stereo, core))
    return nullptr;
  return context;
}
#endif
