// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/GL/GLContext.h"

#include <memory>

#ifdef _WIN32
#include "Common/GL/GLInterface/WGL.h"
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
}  // namespace

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
