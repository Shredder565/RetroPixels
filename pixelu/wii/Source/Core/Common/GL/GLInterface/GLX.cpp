// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/GL/GLInterface/GLX.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

#include "Common/DynamicLibrary.h"
#include "Common/GL/GLInterface/X11Window.h"
#include "Common/Logging/Log.h"

// GLX as specified by Khronos (GLX 1.4, GLX_ARB_create_context(_profile), GLX_EXT_swap_control,
// GLX_MESA_swap_control). <GL/glx.h> is not used: it drags in <GL/gl.h>, whose prototypes clash
// with the function pointers of Common/GL/GLExtensions. Only the types and tokens this file
// needs are declared, with the registry's values.
namespace
{
using GLXFBConfig = struct RpGLXFBConfigRec*;
using GLXContext = struct RpGLXContextRec*;
using GLXDrawable = XID;
using GLXPbuffer = XID;
using GLXWindow = XID;

constexpr int GLX_TOKEN_DOUBLEBUFFER = 5;
constexpr int GLX_TOKEN_STEREO = 6;
constexpr int GLX_TOKEN_RED_SIZE = 8;
constexpr int GLX_TOKEN_GREEN_SIZE = 9;
constexpr int GLX_TOKEN_BLUE_SIZE = 10;
constexpr int GLX_TOKEN_DEPTH_SIZE = 12;
constexpr int GLX_TOKEN_STENCIL_SIZE = 13;
constexpr int GLX_TOKEN_X_VISUAL_TYPE = 0x22;
constexpr int GLX_TOKEN_TRUE_COLOR = 0x8002;
constexpr int GLX_TOKEN_DRAWABLE_TYPE = 0x8010;
constexpr int GLX_TOKEN_RENDER_TYPE = 0x8011;
constexpr int GLX_TOKEN_X_RENDERABLE = 0x8012;
constexpr int GLX_TOKEN_RGBA_TYPE = 0x8014;
constexpr int GLX_TOKEN_PBUFFER_HEIGHT = 0x8040;
constexpr int GLX_TOKEN_PBUFFER_WIDTH = 0x8041;
constexpr int GLX_TOKEN_WINDOW_BIT = 0x0001;
constexpr int GLX_TOKEN_PBUFFER_BIT = 0x0004;
constexpr int GLX_TOKEN_RGBA_BIT = 0x0001;

constexpr int GLX_TOKEN_CONTEXT_MAJOR_VERSION = 0x2091;
constexpr int GLX_TOKEN_CONTEXT_MINOR_VERSION = 0x2092;
constexpr int GLX_TOKEN_CONTEXT_FLAGS = 0x2094;
constexpr int GLX_TOKEN_CONTEXT_PROFILE_MASK = 0x9126;
constexpr int GLX_CONTEXT_CORE_PROFILE = 0x0001;
constexpr int GLX_CONTEXT_FORWARD_COMPATIBLE = 0x0002;
#ifdef _DEBUG
constexpr int GLX_CONTEXT_DEBUG = 0x0001;
constexpr int CORE_CONTEXT_FLAGS = GLX_CONTEXT_FORWARD_COMPATIBLE | GLX_CONTEXT_DEBUG;
#else
constexpr int CORE_CONTEXT_FLAGS = GLX_CONTEXT_FORWARD_COMPATIBLE;
#endif

struct GlxFunctions
{
  Bool (*query_version)(Display*, int*, int*);
  const char* (*query_extensions_string)(Display*, int);
  GLXFBConfig* (*choose_fb_config)(Display*, int, const int*, int*);
  XVisualInfo* (*get_visual_from_fb_config)(Display*, GLXFBConfig);
  GLXContext (*create_new_context)(Display*, GLXFBConfig, int, GLXContext, Bool);
  void (*destroy_context)(Display*, GLXContext);
  Bool (*make_context_current)(Display*, GLXDrawable, GLXDrawable, GLXContext);
  GLXContext (*get_current_context)();
  void (*swap_buffers)(Display*, GLXDrawable);
  GLXWindow (*create_window)(Display*, GLXFBConfig, Window, const int*);
  void (*destroy_window)(Display*, GLXWindow);
  GLXPbuffer (*create_pbuffer)(Display*, GLXFBConfig, const int*);
  void (*destroy_pbuffer)(Display*, GLXPbuffer);
  void* (*get_proc_address)(const unsigned char*);

  // Extensions, found through get_proc_address once the display is known to offer them.
  GLXContext (*create_context_attribs)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
  void (*swap_interval_ext)(Display*, GLXDrawable, int);
  int (*swap_interval_mesa)(unsigned int);
};

GlxFunctions s_glx{};
Common::DynamicLibrary s_libgl;

template <typename Fn>
bool Resolve(const char* name, Fn* out)
{
  *out = reinterpret_cast<Fn>(s_libgl.GetSymbolAddress(name));
  if (*out == nullptr)
    ERROR_LOG_FMT(VIDEO, "GLX: {} is missing from the GL library", name);
  return *out != nullptr;
}

// Opens libGL once (libGLX.so.0 is the GLVND name of the same thing).
bool LoadGlx()
{
  static const bool loaded = [] {
    if (!s_libgl.Open(Common::DynamicLibrary::GetVersionedFilename("GL", 1).c_str()) &&
        !s_libgl.Open(Common::DynamicLibrary::GetVersionedFilename("GLX", 0).c_str()))
    {
      ERROR_LOG_FMT(VIDEO, "GLX: neither libGL.so.1 nor libGLX.so.0 could be loaded");
      return false;
    }
    bool all = true;
    all &= Resolve("glXQueryVersion", &s_glx.query_version);
    all &= Resolve("glXQueryExtensionsString", &s_glx.query_extensions_string);
    all &= Resolve("glXChooseFBConfig", &s_glx.choose_fb_config);
    all &= Resolve("glXGetVisualFromFBConfig", &s_glx.get_visual_from_fb_config);
    all &= Resolve("glXCreateNewContext", &s_glx.create_new_context);
    all &= Resolve("glXDestroyContext", &s_glx.destroy_context);
    all &= Resolve("glXMakeContextCurrent", &s_glx.make_context_current);
    all &= Resolve("glXGetCurrentContext", &s_glx.get_current_context);
    all &= Resolve("glXSwapBuffers", &s_glx.swap_buffers);
    all &= Resolve("glXCreateWindow", &s_glx.create_window);
    all &= Resolve("glXDestroyWindow", &s_glx.destroy_window);
    all &= Resolve("glXCreatePbuffer", &s_glx.create_pbuffer);
    all &= Resolve("glXDestroyPbuffer", &s_glx.destroy_pbuffer);
    all &= Resolve("glXGetProcAddressARB", &s_glx.get_proc_address);
    return all;
  }();
  return loaded;
}

template <typename Fn>
void FetchExtension(const char* name, Fn* out)
{
  *out = reinterpret_cast<Fn>(s_glx.get_proc_address(reinterpret_cast<const unsigned char*>(name)));
}

bool HasExtension(std::string_view list, std::string_view name)
{
  // The list is space separated; a match must be a whole word.
  size_t pos = 0;
  while ((pos = list.find(name, pos)) != std::string_view::npos)
  {
    const size_t end = pos + name.size();
    if ((pos == 0 || list[pos - 1] == ' ') && (end == list.size() || list[end] == ' '))
      return true;
    pos = end;
  }
  return false;
}

// The window config: true colour RGB8, double buffered, no depth or stencil (the backend
// renders into its own framebuffers). Among the matches the first with a 24-bit visual wins, so
// that a compositor does not treat the window's unused alpha as transparency.
GLXFBConfig ChooseWindowConfig(Display* display, int screen, bool stereo, XVisualInfo* out_visual)
{
  const std::array<int, 23> attribs{
      GLX_TOKEN_X_RENDERABLE,
      True,
      GLX_TOKEN_DRAWABLE_TYPE,
      GLX_TOKEN_WINDOW_BIT,
      GLX_TOKEN_RENDER_TYPE,
      GLX_TOKEN_RGBA_BIT,
      GLX_TOKEN_X_VISUAL_TYPE,
      GLX_TOKEN_TRUE_COLOR,
      GLX_TOKEN_RED_SIZE,
      8,
      GLX_TOKEN_GREEN_SIZE,
      8,
      GLX_TOKEN_BLUE_SIZE,
      8,
      GLX_TOKEN_DEPTH_SIZE,
      0,
      GLX_TOKEN_STENCIL_SIZE,
      0,
      GLX_TOKEN_DOUBLEBUFFER,
      True,
      GLX_TOKEN_STEREO,
      stereo ? True : False,
      None,
  };

  int count = 0;
  GLXFBConfig* const configs = s_glx.choose_fb_config(display, screen, attribs.data(), &count);
  if (configs == nullptr || count == 0)
    return nullptr;

  GLXFBConfig chosen = nullptr;
  for (int pass = 0; pass < 2 && chosen == nullptr; ++pass)
  {
    for (int i = 0; i < count && chosen == nullptr; ++i)
    {
      XVisualInfo* const visual = s_glx.get_visual_from_fb_config(display, configs[i]);
      if (visual == nullptr)
        continue;
      // First pass: 24-bit visuals only; second pass: anything with a visual.
      if (pass == 1 || visual->depth == 24)
      {
        chosen = configs[i];
        *out_visual = *visual;
      }
      GLX11::Xlib()->free(visual);
    }
  }
  GLX11::Xlib()->free(configs);
  return chosen;
}

GLXFBConfig ChoosePbufferConfig(Display* display, int screen)
{
  static constexpr std::array<int, 13> attribs{
      GLX_TOKEN_DRAWABLE_TYPE,
      GLX_TOKEN_PBUFFER_BIT,
      GLX_TOKEN_RENDER_TYPE,
      GLX_TOKEN_RGBA_BIT,
      GLX_TOKEN_RED_SIZE,
      8,
      GLX_TOKEN_GREEN_SIZE,
      8,
      GLX_TOKEN_BLUE_SIZE,
      8,
      GLX_TOKEN_DOUBLEBUFFER,
      False,
      None,
  };
  int count = 0;
  GLXFBConfig* const configs = s_glx.choose_fb_config(display, screen, attribs.data(), &count);
  if (configs == nullptr || count == 0)
    return nullptr;
  const GLXFBConfig chosen = configs[0];
  GLX11::Xlib()->free(configs);
  return chosen;
}

GLXFBConfig AsConfig(void* config)
{
  return static_cast<GLXFBConfig>(config);
}

GLXContext AsContext(void* context)
{
  return static_cast<GLXContext>(context);
}
}  // namespace

GLContextGLX::~GLContextGLX()
{
  if (m_target == nullptr)
    return;

  std::lock_guard guard(m_target->Lock());
  Display* const display = m_target->GetDisplay();
  if (m_context != nullptr)
  {
    if (s_glx.get_current_context() == AsContext(m_context))
      s_glx.make_context_current(display, None, None, nullptr);
    s_glx.destroy_context(display, AsContext(m_context));
    m_context = nullptr;
  }
  if (m_drawable != 0)
  {
    if (m_drawable_is_pbuffer)
      s_glx.destroy_pbuffer(display, m_drawable);
    else
      s_glx.destroy_window(display, m_drawable);
    m_drawable = 0;
  }
}

bool GLContextGLX::IsHeadless() const
{
  // Shared contexts have no window, but they are never asked.
  return false;
}

void* GLContextGLX::MakeContext(void* config, void* share, bool core, bool allow_legacy)
{
  Display* const display = m_target->GetDisplay();

  if (core && s_glx.create_context_attribs != nullptr)
  {
    // Versions the driver cannot make fail with an X error each; those are expected here.
    GLX11::ExpectErrors expected;
    for (const GLVersion& want : CORE_VERSIONS_NEWEST_FIRST)
    {
      const std::array<int, 9> attribs{
          GLX_TOKEN_CONTEXT_PROFILE_MASK,
          GLX_CONTEXT_CORE_PROFILE,
          GLX_TOKEN_CONTEXT_FLAGS,
          CORE_CONTEXT_FLAGS,
          GLX_TOKEN_CONTEXT_MAJOR_VERSION,
          want.major,
          GLX_TOKEN_CONTEXT_MINOR_VERSION,
          want.minor,
          None,
      };
      const GLXContext context = s_glx.create_context_attribs(
          display, AsConfig(config), AsContext(share), True, attribs.data());
      GLX11::Xlib()->sync(display, False);
      if (context != nullptr)
      {
        INFO_LOG_FMT(VIDEO, "GLX: Created a GL {}.{} core context", want.major, want.minor);
        m_core_profile = true;
        return context;
      }
    }
    WARN_LOG_FMT(VIDEO, "GLX: Unable to create a core OpenGL context of an acceptable version");
  }
  else if (core)
  {
    WARN_LOG_FMT(VIDEO, "GLX: Missing GLX_ARB_create_context extension");
  }

  if (!allow_legacy)
    return nullptr;

  const GLXContext legacy = s_glx.create_new_context(display, AsConfig(config), GLX_TOKEN_RGBA_TYPE,
                                                     AsContext(share), True);
  if (legacy != nullptr && core)
    WARN_LOG_FMT(VIDEO, "Failed to create a core OpenGL context. Using fallback context.");
  return legacy;
}

bool GLContextGLX::Initialize(const WindowSystemInfo& wsi, bool stereo, bool core)
{
  const auto parent = static_cast<Window>(reinterpret_cast<uintptr_t>(wsi.render_surface));
  if (parent == 0 || !LoadGlx())
    return false;

  m_target = GLX11::Target::Open(wsi.display_connection, parent);
  if (m_target == nullptr)
    return false;

  // GLX is desktop GL only.
  m_mode = Mode::OpenGL;

  {
    std::lock_guard guard(m_target->Lock());
    Display* const display = m_target->GetDisplay();

    int major = 0;
    int minor = 0;
    if (!s_glx.query_version(display, &major, &minor) || major < 1 || (major == 1 && minor < 3))
    {
      ERROR_LOG_FMT(VIDEO, "GLX: version 1.3 or newer is needed, the server has {}.{}", major,
                    minor);
      return false;
    }

    const char* const extensions = s_glx.query_extensions_string(display, m_target->GetScreen());
    const std::string_view ext_list = extensions != nullptr ? extensions : "";
    s_glx.create_context_attribs = nullptr;
    s_glx.swap_interval_ext = nullptr;
    s_glx.swap_interval_mesa = nullptr;
    if (HasExtension(ext_list, "GLX_ARB_create_context"))
      FetchExtension("glXCreateContextAttribsARB", &s_glx.create_context_attribs);
    if (HasExtension(ext_list, "GLX_EXT_swap_control"))
      FetchExtension("glXSwapIntervalEXT", &s_glx.swap_interval_ext);
    if (HasExtension(ext_list, "GLX_MESA_swap_control"))
      FetchExtension("glXSwapIntervalMESA", &s_glx.swap_interval_mesa);

    XVisualInfo visual{};
    const GLXFBConfig config = ChooseWindowConfig(display, m_target->GetScreen(), stereo, &visual);
    if (config == nullptr)
    {
      ERROR_LOG_FMT(VIDEO, "GLX: no suitable framebuffer config{}", stereo ? " (stereo)" : "");
      return false;
    }
    m_config = config;

    if (!m_target->CreateChild(visual))
      return false;
    m_target->FollowParent(&m_surface_width, &m_surface_height);

    m_drawable = s_glx.create_window(display, config, m_target->GetWindow(), nullptr);
    if (m_drawable == 0)
    {
      ERROR_LOG_FMT(VIDEO, "GLX: glXCreateWindow failed");
      return false;
    }

    m_context = MakeContext(config, nullptr, core, true);
    if (m_context == nullptr)
    {
      ERROR_LOG_FMT(VIDEO, "GLX: could not create an OpenGL context");
      return false;
    }
  }

  return MakeCurrent();
}

std::unique_ptr<GLContext> GLContextGLX::CreateSharedContext()
{
  std::lock_guard guard(m_target->Lock());
  Display* const display = m_target->GetDisplay();

  auto shared = std::make_unique<GLContextGLX>();
  shared->m_target = m_target;
  shared->m_mode = m_mode;
  shared->m_shared = true;

  // A 1x1 pbuffer to make the worker's context current with. Without one, a GL 3.0+ context may
  // be current with no drawable at all (GLX_ARB_create_context), so the window config is used.
  if (const GLXFBConfig pbuffer_config = ChoosePbufferConfig(display, m_target->GetScreen()))
  {
    static constexpr std::array<int, 5> size{GLX_TOKEN_PBUFFER_WIDTH, 1, GLX_TOKEN_PBUFFER_HEIGHT,
                                             1, None};
    const GLXPbuffer pbuffer = s_glx.create_pbuffer(display, pbuffer_config, size.data());
    if (pbuffer != 0)
    {
      shared->m_drawable = pbuffer;
      shared->m_drawable_is_pbuffer = true;
      shared->m_config = pbuffer_config;
    }
  }
  if (shared->m_config == nullptr)
    shared->m_config = m_config;

  // Same kind of context as the main one; a legacy main context gets a legacy worker.
  shared->m_context = shared->MakeContext(shared->m_config, m_context, m_core_profile, true);
  if (shared->m_context == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "GLX: could not create a shared context");
    return nullptr;
  }
  return shared;
}

bool GLContextGLX::MakeCurrent()
{
  std::lock_guard guard(m_target->Lock());
  return s_glx.make_context_current(m_target->GetDisplay(), m_drawable, m_drawable,
                                    AsContext(m_context)) == True;
}

bool GLContextGLX::ClearCurrent()
{
  std::lock_guard guard(m_target->Lock());
  return s_glx.make_context_current(m_target->GetDisplay(), None, None, nullptr) == True;
}

void GLContextGLX::Update()
{
  if (m_shared)
    return;
  std::lock_guard guard(m_target->Lock());
  m_target->FollowParent(&m_surface_width, &m_surface_height);
}

void GLContextGLX::Swap()
{
  std::lock_guard guard(m_target->Lock());
  s_glx.swap_buffers(m_target->GetDisplay(), m_drawable);
}

void GLContextGLX::SwapInterval(int interval)
{
  std::lock_guard guard(m_target->Lock());
  if (s_glx.swap_interval_ext != nullptr)
    s_glx.swap_interval_ext(m_target->GetDisplay(), m_drawable, interval);
  else if (s_glx.swap_interval_mesa != nullptr)
    s_glx.swap_interval_mesa(static_cast<unsigned int>(interval));
  else
    ERROR_LOG_FMT(VIDEO,
                  "No support for SwapInterval (framerate clamped to monitor refresh rate).");
}

void* GLContextGLX::GetFuncAddress(const std::string& name)
{
  return s_glx.get_proc_address(reinterpret_cast<const unsigned char*>(name.c_str()));
}
