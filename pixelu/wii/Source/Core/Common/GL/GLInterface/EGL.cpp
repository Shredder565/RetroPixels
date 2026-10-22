// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/GL/GLInterface/EGL.h"

// X11 first: EGL's platform header may use Xlib's types for its native handles.
#include "Common/GL/GLInterface/X11Window.h"

#define EGL_NO_X11
#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

#include "Common/DynamicLibrary.h"
#include "Common/Logging/Log.h"

namespace
{
// Extension tokens, with the values of the Khronos registry (EGL_KHR_create_context,
// EGL_KHR_platform_x11/wayland, EGL_MESA_platform_surfaceless). <EGL/eglext.h> is not used
// because its contents differ a lot between the versions distributions ship.
constexpr EGLint TOKEN_CONTEXT_MAJOR_VERSION = 0x3098;
constexpr EGLint TOKEN_CONTEXT_MINOR_VERSION = 0x30FB;
constexpr EGLint TOKEN_CONTEXT_FLAGS = 0x30FC;
constexpr EGLint TOKEN_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD;
constexpr EGLint CONTEXT_OPENGL_CORE_PROFILE = 0x0001;
constexpr EGLint CONTEXT_OPENGL_FORWARD_COMPATIBLE = 0x0002;
#ifdef _DEBUG
constexpr EGLint CONTEXT_OPENGL_DEBUG = 0x0001;
constexpr EGLint CORE_CONTEXT_FLAGS = CONTEXT_OPENGL_FORWARD_COMPATIBLE | CONTEXT_OPENGL_DEBUG;
#else
constexpr EGLint CORE_CONTEXT_FLAGS = CONTEXT_OPENGL_FORWARD_COMPATIBLE;
#endif
constexpr EGLint RENDERABLE_OPENGL_ES3 = 0x0040;
constexpr EGLenum PLATFORM_X11 = 0x31D5;
constexpr EGLenum PLATFORM_WAYLAND = 0x31D8;
constexpr EGLenum PLATFORM_SURFACELESS = 0x31DD;

// OpenGL ES versions to ask for, newest first. 3.0 is the floor (the backend's GLSL ES 3.00
// shaders and integer attributes).
constexpr std::array<std::array<EGLint, 2>, 3> ES_VERSIONS_NEWEST_FIRST{{{3, 2}, {3, 1}, {3, 0}}};

struct EglFunctions
{
  __eglMustCastToProperFunctionPointerType (*get_proc_address)(const char*);
  EGLint (*get_error)();
  EGLDisplay (*get_display)(EGLNativeDisplayType);
  EGLBoolean (*initialize)(EGLDisplay, EGLint*, EGLint*);
  EGLBoolean (*terminate)(EGLDisplay);
  const char* (*query_string)(EGLDisplay, EGLint);
  EGLBoolean (*bind_api)(EGLenum);
  EGLBoolean (*choose_config)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
  EGLBoolean (*get_config_attrib)(EGLDisplay, EGLConfig, EGLint, EGLint*);
  EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
  EGLBoolean (*destroy_context)(EGLDisplay, EGLContext);
  EGLSurface (*create_window_surface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*);
  EGLSurface (*create_pbuffer_surface)(EGLDisplay, EGLConfig, const EGLint*);
  EGLBoolean (*destroy_surface)(EGLDisplay, EGLSurface);
  EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
  EGLContext (*get_current_context)();
  EGLBoolean (*swap_buffers)(EGLDisplay, EGLSurface);
  EGLBoolean (*swap_interval)(EGLDisplay, EGLint);
  EGLBoolean (*release_thread)();
  // EGL 1.5 or EGL_EXT_platform_base (same arguments, attributes as EGLint for the EXT one).
  EGLDisplay (*get_platform_display)(EGLenum, void*, const intptr_t*);
  EGLDisplay (*get_platform_display_ext)(EGLenum, void*, const EGLint*);
};

EglFunctions s_egl{};
Common::DynamicLibrary s_libegl;
// GL entry points eglGetProcAddress does not return (EGL before 1.5 only hands out extension
// functions) are looked up in the client API library itself.
Common::DynamicLibrary s_libgl_desktop;
Common::DynamicLibrary s_libgl_es;

template <typename Fn>
bool Resolve(const char* name, Fn* out, bool required = true)
{
  *out = reinterpret_cast<Fn>(s_libegl.GetSymbolAddress(name));
  if (*out == nullptr && required)
    ERROR_LOG_FMT(VIDEO, "EGL: {} is missing from libEGL", name);
  return *out != nullptr || !required;
}

bool LoadEgl()
{
  static const bool loaded = [] {
    if (!s_libegl.Open(Common::DynamicLibrary::GetVersionedFilename("EGL", 1).c_str()))
    {
      ERROR_LOG_FMT(VIDEO, "EGL: libEGL.so.1 could not be loaded");
      return false;
    }
    bool all = true;
    all &= Resolve("eglGetProcAddress", &s_egl.get_proc_address);
    all &= Resolve("eglGetError", &s_egl.get_error);
    all &= Resolve("eglGetDisplay", &s_egl.get_display);
    all &= Resolve("eglInitialize", &s_egl.initialize);
    all &= Resolve("eglTerminate", &s_egl.terminate);
    all &= Resolve("eglQueryString", &s_egl.query_string);
    all &= Resolve("eglBindAPI", &s_egl.bind_api);
    all &= Resolve("eglChooseConfig", &s_egl.choose_config);
    all &= Resolve("eglGetConfigAttrib", &s_egl.get_config_attrib);
    all &= Resolve("eglCreateContext", &s_egl.create_context);
    all &= Resolve("eglDestroyContext", &s_egl.destroy_context);
    all &= Resolve("eglCreateWindowSurface", &s_egl.create_window_surface);
    all &= Resolve("eglCreatePbufferSurface", &s_egl.create_pbuffer_surface);
    all &= Resolve("eglDestroySurface", &s_egl.destroy_surface);
    all &= Resolve("eglMakeCurrent", &s_egl.make_current);
    all &= Resolve("eglGetCurrentContext", &s_egl.get_current_context);
    all &= Resolve("eglSwapBuffers", &s_egl.swap_buffers);
    all &= Resolve("eglSwapInterval", &s_egl.swap_interval);
    all &= Resolve("eglReleaseThread", &s_egl.release_thread);
    Resolve("eglGetPlatformDisplay", &s_egl.get_platform_display, false);
    if (all)
    {
      s_egl.get_platform_display_ext = reinterpret_cast<decltype(s_egl.get_platform_display_ext)>(
          s_egl.get_proc_address("eglGetPlatformDisplayEXT"));
    }
    return all;
  }();
  return loaded;
}

bool HasExtension(const char* list, std::string_view name)
{
  if (list == nullptr)
    return false;
  const std::string_view all{list};
  size_t pos = 0;
  while ((pos = all.find(name, pos)) != std::string_view::npos)
  {
    const size_t end = pos + name.size();
    if ((pos == 0 || all[pos - 1] == ' ') && (end == all.size() || all[end] == ' '))
      return true;
    pos = end;
  }
  return false;
}

// libwayland-egl, the glue between a wl_surface and an EGL window surface.
struct WaylandEglFunctions
{
  void* (*create)(void* surface, int width, int height);
  void (*destroy)(void* window);
  void (*resize)(void* window, int width, int height, int dx, int dy);
};

const WaylandEglFunctions* WaylandEgl()
{
  static Common::DynamicLibrary library;
  static WaylandEglFunctions fns{};
  static const bool loaded = [] {
    if (!library.Open(Common::DynamicLibrary::GetVersionedFilename("wayland-egl", 1).c_str()))
    {
      ERROR_LOG_FMT(VIDEO, "EGL: libwayland-egl.so.1 could not be loaded");
      return false;
    }
    fns.create =
        reinterpret_cast<decltype(fns.create)>(library.GetSymbolAddress("wl_egl_window_create"));
    fns.destroy =
        reinterpret_cast<decltype(fns.destroy)>(library.GetSymbolAddress("wl_egl_window_destroy"));
    fns.resize =
        reinterpret_cast<decltype(fns.resize)>(library.GetSymbolAddress("wl_egl_window_resize"));
    return fns.create != nullptr && fns.destroy != nullptr && fns.resize != nullptr;
  }();
  return loaded ? &fns : nullptr;
}

// A Wayland surface has no size the client could ask for; until the frontend says otherwise
// the window starts at this size.
constexpr u32 WAYLAND_DEFAULT_WIDTH = 640;
constexpr u32 WAYLAND_DEFAULT_HEIGHT = 480;
}  // namespace

namespace GLEGL
{
// The EGL display and, on X11, our own connection and child window. Shared by the main context
// and its worker contexts; the display is terminated when the last of them is gone.
struct Connection
{
  ~Connection()
  {
    if (display != EGL_NO_DISPLAY)
      s_egl.terminate(display);
    // target (X11) closes after EGL let go of it.
  }

  std::mutex& Lock() { return x11 != nullptr ? x11->Lock() : own_lock; }

  WindowSystemType type = WindowSystemType::Headless;
  EGLDisplay display = EGL_NO_DISPLAY;
  std::unique_ptr<GLX11::Target> x11;
  void* wayland_surface = nullptr;
  bool surfaceless_contexts = false;
  bool create_context_ext = false;
  std::mutex own_lock;
};
}  // namespace GLEGL

namespace
{
EGLDisplay OpenDisplay(GLEGL::Connection* connection, void* native_display)
{
  const char* const client_extensions = s_egl.query_string(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  // Drop the error a pre-1.5 EGL raises for the EGL_NO_DISPLAY query.
  s_egl.get_error();

  EGLenum platform = 0;
  const char* platform_extension = nullptr;
  switch (connection->type)
  {
  case WindowSystemType::X11:
    platform = PLATFORM_X11;
    platform_extension = HasExtension(client_extensions, "EGL_KHR_platform_x11") ?
                             "EGL_KHR_platform_x11" :
                             "EGL_EXT_platform_x11";
    break;
  case WindowSystemType::Wayland:
    platform = PLATFORM_WAYLAND;
    platform_extension = HasExtension(client_extensions, "EGL_KHR_platform_wayland") ?
                             "EGL_KHR_platform_wayland" :
                             "EGL_EXT_platform_wayland";
    break;
  default:
    platform = PLATFORM_SURFACELESS;
    platform_extension = "EGL_MESA_platform_surfaceless";
    break;
  }

  if (HasExtension(client_extensions, platform_extension))
  {
    EGLDisplay display = EGL_NO_DISPLAY;
    if (s_egl.get_platform_display != nullptr)
      display = s_egl.get_platform_display(platform, native_display, nullptr);
    else if (s_egl.get_platform_display_ext != nullptr)
      display = s_egl.get_platform_display_ext(platform, native_display, nullptr);
    if (display != EGL_NO_DISPLAY)
    {
      INFO_LOG_FMT(VIDEO, "EGL: display from {}", platform_extension);
      return display;
    }
  }

  // The implementation guesses the platform of a native display it is handed directly; for
  // headless use that is its default display.
  if (connection->type == WindowSystemType::Headless)
    return s_egl.get_display(EGL_DEFAULT_DISPLAY);
  return s_egl.get_display(reinterpret_cast<EGLNativeDisplayType>(native_display));
}

// The first config (in EGL's order, best first) that renders with `renderable` and, for a
// window on X11, prefers a 24-bit visual so a compositor does not show the unused alpha.
EGLConfig ChooseConfig(GLEGL::Connection* connection, EGLint renderable, EGLint surface_type)
{
  const std::array<EGLint, 15> attribs{
      EGL_SURFACE_TYPE,
      surface_type,
      EGL_RENDERABLE_TYPE,
      renderable,
      EGL_RED_SIZE,
      8,
      EGL_GREEN_SIZE,
      8,
      EGL_BLUE_SIZE,
      8,
      EGL_DEPTH_SIZE,
      0,
      EGL_STENCIL_SIZE,
      0,
      EGL_NONE,
  };
  EGLint count = 0;
  if (!s_egl.choose_config(connection->display, attribs.data(), nullptr, 0, &count) || count == 0)
    return nullptr;
  std::vector<EGLConfig> configs(static_cast<size_t>(count));
  s_egl.choose_config(connection->display, attribs.data(), configs.data(), count, &count);
  configs.resize(static_cast<size_t>(count));
  if (configs.empty())
    return nullptr;

  if (connection->type != WindowSystemType::X11)
  {
    // Without an X visual to look at, prefer a config without alpha (Wayland compositors blend
    // with it).
    for (EGLConfig config : configs)
    {
      EGLint alpha = 0;
      if (s_egl.get_config_attrib(connection->display, config, EGL_ALPHA_SIZE, &alpha) &&
          alpha == 0)
      {
        return config;
      }
    }
    return configs[0];
  }

  EGLConfig fallback = nullptr;
  for (EGLConfig config : configs)
  {
    EGLint visual_id = 0;
    XVisualInfo visual{};
    if (!s_egl.get_config_attrib(connection->display, config, EGL_NATIVE_VISUAL_ID, &visual_id) ||
        visual_id == 0 || !connection->x11->FindVisual(static_cast<VisualID>(visual_id), &visual))
    {
      continue;
    }
    if (visual.depth == 24)
      return config;
    if (fallback == nullptr)
      fallback = config;
  }
  return fallback;
}
}  // namespace

GLContextEGL::~GLContextEGL()
{
  if (m_connection == nullptr)
    return;

  std::lock_guard guard(m_connection->Lock());
  const EGLDisplay display = m_connection->display;
  if (m_context != nullptr)
  {
    if (s_egl.get_current_context() == m_context)
      s_egl.make_current(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    s_egl.destroy_context(display, m_context);
    m_context = nullptr;
  }
  if (m_surface != nullptr)
  {
    s_egl.destroy_surface(display, m_surface);
    m_surface = nullptr;
  }
  if (m_wayland_window != nullptr)
  {
    WaylandEgl()->destroy(m_wayland_window);
    m_wayland_window = nullptr;
  }
}

bool GLContextEGL::IsHeadless() const
{
  return m_connection->type == WindowSystemType::Headless;
}

bool GLContextEGL::CreateContextFor(Mode api, bool core)
{
  GLEGL::Connection* const connection = m_connection.get();
  const bool es = api == Mode::OpenGLES;
  if (!s_egl.bind_api(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API))
    return false;

  // Window surfaces for the main context on a window system, pbuffers when headless and the
  // implementation cannot do without a surface.
  EGLint surface_type = EGL_WINDOW_BIT;
  if (connection->type == WindowSystemType::Headless)
    surface_type = connection->surfaceless_contexts ? 0 : EGL_PBUFFER_BIT;

  const EGLint renderable = es ? RENDERABLE_OPENGL_ES3 : EGL_OPENGL_BIT;
  m_config = ChooseConfig(connection, renderable, surface_type);
  if (m_config == nullptr)
  {
    INFO_LOG_FMT(VIDEO, "EGL: no config for {}", es ? "OpenGL ES 3" : "OpenGL");
    return false;
  }

  m_core_profile = false;
  m_version_major = 0;
  m_version_minor = 0;
  if (es)
  {
    for (const auto& [major, minor] : ES_VERSIONS_NEWEST_FIRST)
    {
      // Without EGL_KHR_create_context only the major version can be named.
      if (!connection->create_context_ext && minor != 0)
        continue;
      const std::array<EGLint, 5> attribs{TOKEN_CONTEXT_MAJOR_VERSION, major,
                                          TOKEN_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
      const std::array<EGLint, 3> major_only{TOKEN_CONTEXT_MAJOR_VERSION, major, EGL_NONE};
      m_context =
          s_egl.create_context(connection->display, m_config, EGL_NO_CONTEXT,
                               connection->create_context_ext ? attribs.data() : major_only.data());
      if (m_context != EGL_NO_CONTEXT)
      {
        INFO_LOG_FMT(VIDEO, "EGL: Created an OpenGL ES {}.{} context", major, minor);
        m_version_major = major;
        m_version_minor = minor;
        return true;
      }
    }
    return false;
  }

  if (core && connection->create_context_ext)
  {
    for (const GLVersion& want : CORE_VERSIONS_NEWEST_FIRST)
    {
      const std::array<EGLint, 9> attribs{
          TOKEN_CONTEXT_OPENGL_PROFILE_MASK,
          CONTEXT_OPENGL_CORE_PROFILE,
          TOKEN_CONTEXT_FLAGS,
          CORE_CONTEXT_FLAGS,
          TOKEN_CONTEXT_MAJOR_VERSION,
          want.major,
          TOKEN_CONTEXT_MINOR_VERSION,
          want.minor,
          EGL_NONE,
      };
      m_context =
          s_egl.create_context(connection->display, m_config, EGL_NO_CONTEXT, attribs.data());
      if (m_context != EGL_NO_CONTEXT)
      {
        INFO_LOG_FMT(VIDEO, "EGL: Created a GL {}.{} core context", want.major, want.minor);
        m_core_profile = true;
        m_version_major = want.major;
        m_version_minor = want.minor;
        return true;
      }
    }
    WARN_LOG_FMT(VIDEO, "EGL: Unable to create a core OpenGL context of an acceptable version");
  }

  m_context = s_egl.create_context(connection->display, m_config, EGL_NO_CONTEXT, nullptr);
  if (m_context == EGL_NO_CONTEXT)
    return false;
  if (core)
    WARN_LOG_FMT(VIDEO, "Failed to create a core OpenGL context. Using fallback context.");
  return true;
}

bool GLContextEGL::CreateWindowSurface()
{
  GLEGL::Connection* const connection = m_connection.get();
  switch (connection->type)
  {
  case WindowSystemType::X11:
  {
    EGLint visual_id = 0;
    XVisualInfo visual{};
    if (!s_egl.get_config_attrib(connection->display, m_config, EGL_NATIVE_VISUAL_ID, &visual_id) ||
        !connection->x11->FindVisual(static_cast<VisualID>(visual_id), &visual) ||
        !connection->x11->CreateChild(visual))
    {
      return false;
    }
    connection->x11->FollowParent(&m_surface_width, &m_surface_height);
    m_surface = s_egl.create_window_surface(
        connection->display, m_config,
        static_cast<EGLNativeWindowType>(connection->x11->GetWindow()), nullptr);
    break;
  }
  case WindowSystemType::Wayland:
  {
    const WaylandEglFunctions* wl = WaylandEgl();
    if (wl == nullptr)
      return false;
    if (m_surface_width == 0 || m_surface_height == 0)
    {
      m_surface_width = WAYLAND_DEFAULT_WIDTH;
      m_surface_height = WAYLAND_DEFAULT_HEIGHT;
    }
    m_wayland_window = wl->create(connection->wayland_surface, static_cast<int>(m_surface_width),
                                  static_cast<int>(m_surface_height));
    if (m_wayland_window == nullptr)
      return false;
    m_surface = s_egl.create_window_surface(
        connection->display, m_config,
        static_cast<EGLNativeWindowType>(reinterpret_cast<uintptr_t>(m_wayland_window)), nullptr);
    break;
  }
  default:
    // Headless: surfaceless where possible, else a pbuffer standing in for a window.
    m_surface_width = 1;
    m_surface_height = 1;
    if (connection->surfaceless_contexts)
      return true;
    {
      static constexpr std::array<EGLint, 5> size{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
      m_surface = s_egl.create_pbuffer_surface(connection->display, m_config, size.data());
    }
    break;
  }

  if (m_surface == EGL_NO_SURFACE)
  {
    ERROR_LOG_FMT(VIDEO, "EGL: could not create the surface (error {:#x})", s_egl.get_error());
    return false;
  }
  return true;
}

bool GLContextEGL::Initialize(const WindowSystemInfo& wsi, bool stereo, bool core)
{
  if (!LoadEgl())
    return false;

  auto connection = std::make_shared<GLEGL::Connection>();
  connection->type = wsi.type;
  void* native_display = nullptr;
  switch (wsi.type)
  {
  case WindowSystemType::X11:
  {
    const auto parent = static_cast<Window>(reinterpret_cast<uintptr_t>(wsi.render_surface));
    if (parent == 0)
      return false;
    connection->x11 = GLX11::Target::Open(wsi.display_connection, parent);
    if (connection->x11 == nullptr)
      return false;
    native_display = connection->x11->GetDisplay();
    break;
  }
  case WindowSystemType::Wayland:
    if (wsi.display_connection == nullptr || wsi.render_surface == nullptr)
      return false;
    native_display = wsi.display_connection;
    connection->wayland_surface = wsi.render_surface;
    break;
  case WindowSystemType::Headless:
    break;
  default:
    return false;
  }
  m_connection = connection;

  std::lock_guard guard(connection->Lock());
  connection->display = OpenDisplay(connection.get(), native_display);
  EGLint major = 0;
  EGLint minor = 0;
  if (connection->display == EGL_NO_DISPLAY ||
      !s_egl.initialize(connection->display, &major, &minor))
  {
    ERROR_LOG_FMT(VIDEO, "EGL: could not open a display (error {:#x})", s_egl.get_error());
    connection->display = EGL_NO_DISPLAY;
    return false;
  }
  INFO_LOG_FMT(VIDEO, "EGL {}.{}: {}", major, minor,
               s_egl.query_string(connection->display, EGL_VENDOR));

  const char* const extensions = s_egl.query_string(connection->display, EGL_EXTENSIONS);
  connection->surfaceless_contexts = HasExtension(extensions, "EGL_KHR_surfaceless_context");
  connection->create_context_ext =
      HasExtension(extensions, "EGL_KHR_create_context") || major > 1 || minor >= 5;
  // EGL has no stereo config attribute: quad-buffered stereo is not offered through EGL.
  if (stereo)
    WARN_LOG_FMT(VIDEO, "EGL: quad-buffered stereo is not available through EGL");

  // Desktop GL first, unless ES was asked for; the other one if the first cannot be made.
  const Mode first = m_mode == Mode::OpenGLES ? Mode::OpenGLES : Mode::OpenGL;
  const Mode second = first == Mode::OpenGL ? Mode::OpenGLES : Mode::OpenGL;
  if (CreateContextFor(first, core))
    m_mode = first;
  else if (CreateContextFor(second, core))
    m_mode = second;
  else
  {
    ERROR_LOG_FMT(VIDEO, "EGL: could not create an OpenGL or OpenGL ES 3 context");
    return false;
  }

  if (!CreateWindowSurface())
    return false;

  return s_egl.make_current(connection->display, m_surface, m_surface, m_context) == EGL_TRUE;
}

std::unique_ptr<GLContext> GLContextEGL::CreateSharedContext()
{
  GLEGL::Connection* const connection = m_connection.get();
  std::lock_guard guard(connection->Lock());

  auto shared = std::make_unique<GLContextEGL>();
  shared->m_connection = m_connection;
  shared->m_mode = m_mode;
  shared->m_shared = true;
  shared->m_core_profile = m_core_profile;
  shared->m_version_major = m_version_major;
  shared->m_version_minor = m_version_minor;

  if (!s_egl.bind_api(m_mode == Mode::OpenGLES ? EGL_OPENGL_ES_API : EGL_OPENGL_API))
    return nullptr;

  // A worker context needs a surface to be current with unless the implementation allows
  // none. The pbuffer needs a config that can make one.
  EGLConfig config = m_config;
  if (!connection->surfaceless_contexts)
  {
    const EGLint renderable = m_mode == Mode::OpenGLES ? RENDERABLE_OPENGL_ES3 : EGL_OPENGL_BIT;
    config = ChooseConfig(connection, renderable, EGL_PBUFFER_BIT);
    if (config == nullptr)
    {
      ERROR_LOG_FMT(VIDEO, "EGL: no pbuffer config for a shared context");
      return nullptr;
    }
    static constexpr std::array<EGLint, 5> size{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    shared->m_surface = s_egl.create_pbuffer_surface(connection->display, config, size.data());
    if (shared->m_surface == EGL_NO_SURFACE)
    {
      ERROR_LOG_FMT(VIDEO, "EGL: could not create a pbuffer (error {:#x})", s_egl.get_error());
      return nullptr;
    }
  }
  shared->m_config = config;

  // The same kind of context as ours: the version and profile it was made with (a legacy
  // context was made without attributes, and so is its worker).
  std::vector<EGLint> attribs;
  if (m_version_major != 0)
  {
    attribs.insert(attribs.end(), {TOKEN_CONTEXT_MAJOR_VERSION, m_version_major});
    if (connection->create_context_ext)
      attribs.insert(attribs.end(), {TOKEN_CONTEXT_MINOR_VERSION, m_version_minor});
  }
  if (m_core_profile)
  {
    attribs.insert(attribs.end(), {TOKEN_CONTEXT_OPENGL_PROFILE_MASK, CONTEXT_OPENGL_CORE_PROFILE,
                                   TOKEN_CONTEXT_FLAGS, CORE_CONTEXT_FLAGS});
  }
  attribs.push_back(EGL_NONE);

  shared->m_context = s_egl.create_context(connection->display, config, m_context, attribs.data());
  if (shared->m_context == EGL_NO_CONTEXT)
  {
    ERROR_LOG_FMT(VIDEO, "EGL: could not create a shared context (error {:#x})", s_egl.get_error());
    return nullptr;
  }
  return shared;
}

bool GLContextEGL::MakeCurrent()
{
  std::lock_guard guard(m_connection->Lock());
  return s_egl.make_current(m_connection->display, m_surface, m_surface, m_context) == EGL_TRUE;
}

bool GLContextEGL::ClearCurrent()
{
  std::lock_guard guard(m_connection->Lock());
  return s_egl.make_current(m_connection->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                            EGL_NO_CONTEXT) == EGL_TRUE;
}

void GLContextEGL::SetSurfaceSize(u32 width, u32 height)
{
  // Only Wayland needs to be told; X11 windows are measured.
  if (m_connection->type != WindowSystemType::Wayland || width == 0 || height == 0)
    return;
  m_surface_width = width;
  m_surface_height = height;
  m_has_size_from_frontend = true;
}

void GLContextEGL::Update()
{
  if (m_shared)
    return;
  std::lock_guard guard(m_connection->Lock());
  if (m_connection->type == WindowSystemType::X11)
  {
    m_connection->x11->FollowParent(&m_surface_width, &m_surface_height);
  }
  else if (m_connection->type == WindowSystemType::Wayland && m_wayland_window != nullptr &&
           m_has_size_from_frontend)
  {
    WaylandEgl()->resize(m_wayland_window, static_cast<int>(m_surface_width),
                         static_cast<int>(m_surface_height), 0, 0);
  }
}

void GLContextEGL::Swap()
{
  if (m_surface == EGL_NO_SURFACE)
    return;
  std::lock_guard guard(m_connection->Lock());
  s_egl.swap_buffers(m_connection->display, m_surface);
}

void GLContextEGL::SwapInterval(int interval)
{
  // Nothing is presented without a window surface (headless).
  if (m_surface == EGL_NO_SURFACE || IsHeadless())
    return;
  std::lock_guard guard(m_connection->Lock());
  if (!s_egl.swap_interval(m_connection->display, interval))
  {
    ERROR_LOG_FMT(VIDEO,
                  "No support for SwapInterval (framerate clamped to monitor refresh rate).");
  }
}

void* GLContextEGL::GetFuncAddress(const std::string& name)
{
  if (void* const fn = reinterpret_cast<void*>(s_egl.get_proc_address(name.c_str())))
    return fn;

  // Core functions of the client API, which EGL 1.4 does not have to hand out.
  Common::DynamicLibrary& library = m_mode == Mode::OpenGLES ? s_libgl_es : s_libgl_desktop;
  if (!library.IsOpen())
  {
    if (m_mode == Mode::OpenGLES)
      library.Open(Common::DynamicLibrary::GetVersionedFilename("GLESv2", 2).c_str());
    else if (!library.Open(Common::DynamicLibrary::GetVersionedFilename("OpenGL", 0).c_str()))
      library.Open(Common::DynamicLibrary::GetVersionedFilename("GL", 1).c_str());
  }
  return library.IsOpen() ? library.GetSymbolAddress(name.c_str()) : nullptr;
}
