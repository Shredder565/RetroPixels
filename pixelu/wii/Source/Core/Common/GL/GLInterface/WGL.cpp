// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/GL/GLInterface/WGL.h"

#include <windows.h>

#include <array>
#include <string>

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

// The WGL extensions used here, as specified in the Khronos registry (WGL_ARB_pbuffer,
// WGL_ARB_pixel_format, WGL_ARB_create_context(_profile), WGL_EXT_swap_control). Only the
// tokens this file passes are defined.
namespace
{
DECLARE_HANDLE(RpPbufferHandle);

constexpr int WGL_TOKEN_DRAW_TO_PBUFFER = 0x202D;
constexpr int WGL_TOKEN_RED_BITS = 0x2015;
constexpr int WGL_TOKEN_GREEN_BITS = 0x2017;
constexpr int WGL_TOKEN_BLUE_BITS = 0x2019;
constexpr int WGL_TOKEN_DEPTH_BITS = 0x2022;
constexpr int WGL_TOKEN_STENCIL_BITS = 0x2023;

constexpr int WGL_TOKEN_CONTEXT_MAJOR_VERSION = 0x2091;
constexpr int WGL_TOKEN_CONTEXT_MINOR_VERSION = 0x2092;
constexpr int WGL_TOKEN_CONTEXT_FLAGS = 0x2094;
constexpr int WGL_TOKEN_CONTEXT_PROFILE_MASK = 0x9126;
constexpr int WGL_CONTEXT_FLAG_FORWARD_COMPATIBLE = 0x0002;
#ifdef _DEBUG
constexpr int WGL_CONTEXT_FLAG_DEBUG = 0x0001;
constexpr int CORE_CONTEXT_FLAGS = WGL_CONTEXT_FLAG_FORWARD_COMPATIBLE | WGL_CONTEXT_FLAG_DEBUG;
#else
constexpr int CORE_CONTEXT_FLAGS = WGL_CONTEXT_FLAG_FORWARD_COMPATIBLE;
#endif
constexpr int WGL_PROFILE_CORE = 0x0001;

using SwapIntervalFn = BOOL(WINAPI*)(int interval);
using CreateContextAttribsFn = HGLRC(WINAPI*)(HDC dc, HGLRC share_with, const int* attribs);
using ChoosePixelFormatFn = BOOL(WINAPI*)(HDC dc, const int* int_attribs,
                                          const FLOAT* float_attribs, UINT max_formats,
                                          int* formats, UINT* format_count);
using CreatePbufferFn = RpPbufferHandle(WINAPI*)(HDC dc, int pixel_format, int width, int height,
                                                 const int* attribs);
using GetPbufferDCFn = HDC(WINAPI*)(RpPbufferHandle pbuffer);
using ReleasePbufferDCFn = int(WINAPI*)(RpPbufferHandle pbuffer, HDC dc);
using DestroyPbufferFn = BOOL(WINAPI*)(RpPbufferHandle pbuffer);

// Extension entry points. wglGetProcAddress only works with a context current, so they are
// fetched while the temporary legacy context is current and reused for every later context.
struct WglEntryPoints
{
  SwapIntervalFn swap_interval = nullptr;
  CreateContextAttribsFn create_context_attribs = nullptr;
  ChoosePixelFormatFn choose_pixel_format = nullptr;
  CreatePbufferFn create_pbuffer = nullptr;
  GetPbufferDCFn get_pbuffer_dc = nullptr;
  ReleasePbufferDCFn release_pbuffer_dc = nullptr;
  DestroyPbufferFn destroy_pbuffer = nullptr;

  bool HasPbuffers() const
  {
    return choose_pixel_format != nullptr && create_pbuffer != nullptr &&
           get_pbuffer_dc != nullptr && release_pbuffer_dc != nullptr &&
           destroy_pbuffer != nullptr;
  }
};

WglEntryPoints s_wgl;

template <typename Fn>
void Fetch(Fn& out, const char* name)
{
  out = reinterpret_cast<Fn>(wglGetProcAddress(name));
}

void FetchEntryPoints()
{
  Fetch(s_wgl.swap_interval, "wglSwapIntervalEXT");
  Fetch(s_wgl.create_context_attribs, "wglCreateContextAttribsARB");
  Fetch(s_wgl.choose_pixel_format, "wglChoosePixelFormatARB");
  Fetch(s_wgl.create_pbuffer, "wglCreatePbufferARB");
  Fetch(s_wgl.get_pbuffer_dc, "wglGetPbufferDCARB");
  Fetch(s_wgl.release_pbuffer_dc, "wglReleasePbufferDCARB");
  Fetch(s_wgl.destroy_pbuffer, "wglDestroyPbufferARB");
}

RpPbufferHandle AsPbuffer(HANDLE handle)
{
  return static_cast<RpPbufferHandle>(handle);
}

// Makes a width x height pbuffer with a colour-only format compatible with window_dc.
bool MakePbuffer(HDC window_dc, int width, int height, HANDLE* out_pbuffer, HDC* out_dc)
{
  if (!s_wgl.HasPbuffers())
  {
    ERROR_LOG_FMT(VIDEO, "Missing WGL_ARB_pbuffer extension");
    return false;
  }

  // Key/value pairs, zero-terminated. No colour/depth/stencil size is required.
  static constexpr std::array<int, 14> format_attribs{
      WGL_TOKEN_DRAW_TO_PBUFFER, 1, WGL_TOKEN_RED_BITS,     0, WGL_TOKEN_GREEN_BITS,   0,
      WGL_TOKEN_BLUE_BITS,       0, WGL_TOKEN_DEPTH_BITS,   0, WGL_TOKEN_STENCIL_BITS, 0,
      0,                         0,
  };
  static constexpr std::array<float, 2> no_float_attribs{};
  static constexpr std::array<int, 2> no_pbuffer_attribs{};

  int format = 0;
  UINT format_count = 0;
  if (!s_wgl.choose_pixel_format(window_dc, format_attribs.data(), no_float_attribs.data(), 1,
                                 &format, &format_count))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to obtain a compatible pbuffer pixel format");
    return false;
  }

  const RpPbufferHandle pbuffer =
      s_wgl.create_pbuffer(window_dc, format, width, height, no_pbuffer_attribs.data());
  if (pbuffer == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to create pbuffer");
    return false;
  }

  const HDC dc = s_wgl.get_pbuffer_dc(pbuffer);
  if (dc == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to get drawing context from pbuffer");
    s_wgl.destroy_pbuffer(pbuffer);
    return false;
  }

  *out_pbuffer = pbuffer;
  *out_dc = dc;
  return true;
}

// The legacy pixel format for the window: RGBA, 32-bit colour, double buffered, no depth or
// stencil (the backend renders into its own framebuffers). It is built on first use, so the
// stereo flag is the one passed when the first window context was made.
const PIXELFORMATDESCRIPTOR& WindowPixelFormat(bool stereo)
{
  static const PIXELFORMATDESCRIPTOR descriptor = [stereo] {
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER |
                  (stereo ? static_cast<DWORD>(PFD_STEREO) : DWORD{0});
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    return pfd;
  }();
  return descriptor;
}
}  // namespace

GLContextWGL::~GLContextWGL()
{
  if (m_hglrc != nullptr)
  {
    if (wglGetCurrentContext() == m_hglrc && !wglMakeCurrent(m_hdc, nullptr))
      NOTICE_LOG_FMT(VIDEO, "Could not release drawing context.");

    if (!wglDeleteContext(m_hglrc))
      ERROR_LOG_FMT(VIDEO, "Attempt to release rendering context failed.");
    m_hglrc = nullptr;
  }

  ReleaseDrawable();
}

void GLContextWGL::ReleaseDrawable()
{
  if (m_hdc == nullptr)
    return;

  if (m_pbuffer != nullptr)
  {
    s_wgl.release_pbuffer_dc(AsPbuffer(m_pbuffer), m_hdc);
    m_hdc = nullptr;
    s_wgl.destroy_pbuffer(AsPbuffer(m_pbuffer));
    m_pbuffer = nullptr;
    return;
  }

  if (!ReleaseDC(m_hwnd, m_hdc))
    ERROR_LOG_FMT(VIDEO, "Attempt to release device context failed.");
  m_hdc = nullptr;
}

bool GLContextWGL::IsHeadless() const
{
  return m_hwnd == nullptr;
}

void GLContextWGL::SwapInterval(int interval)
{
  if (s_wgl.swap_interval == nullptr)
  {
    ERROR_LOG_FMT(VIDEO,
                  "No support for SwapInterval (framerate clamped to monitor refresh rate).");
    return;
  }
  s_wgl.swap_interval(interval);
}

void GLContextWGL::Swap()
{
  SwapBuffers(m_hdc);
}

void* GLContextWGL::GetFuncAddress(const std::string& name)
{
  // wglGetProcAddress only knows extension and post-1.1 entry points; the GL 1.1 core lives in
  // opengl32.dll itself, which is loaded since this module links against it.
  FARPROC proc = wglGetProcAddress(name.c_str());
  if (proc == nullptr)
    proc = GetProcAddress(GetModuleHandle(TEXT("opengl32.dll")), name.c_str());
  return reinterpret_cast<void*>(proc);
}

bool GLContextWGL::ReadClientSize()
{
  RECT client{};
  if (!GetClientRect(m_hwnd, &client))
    return false;

  m_surface_width = static_cast<u32>(client.right - client.left);
  m_surface_height = static_cast<u32>(client.bottom - client.top);
  return true;
}

bool GLContextWGL::Initialize(const WindowSystemInfo& wsi, bool stereo, bool core)
{
  if (wsi.render_surface == nullptr)
    return false;

  m_hwnd = static_cast<HWND>(wsi.render_surface);
  if (!ReadClientSize())
    return false;

  // Entry points from an earlier context are not trusted for this one.
  s_wgl = {};

  const PIXELFORMATDESCRIPTOR& pfd = WindowPixelFormat(stereo);

  m_hdc = GetDC(m_hwnd);
  if (m_hdc == nullptr)
  {
    PanicAlertFmt("(1) Can't create an OpenGL Device context. Fail.");
    return false;
  }

  const int format = ChoosePixelFormat(m_hdc, &pfd);
  if (format == 0)
  {
    PanicAlertFmt("(2) Can't find a suitable PixelFormat.");
    return false;
  }
  if (!SetPixelFormat(m_hdc, format, &pfd))
  {
    PanicAlertFmt("(3) Can't set the PixelFormat.");
    return false;
  }

  // A legacy context first: it is needed to look up wglCreateContextAttribsARB at all, and it
  // is what we keep if no core profile can be made.
  m_hglrc = wglCreateContext(m_hdc);
  if (m_hglrc == nullptr)
  {
    PanicAlertFmt("(4) Can't create an OpenGL rendering context.");
    return false;
  }

  // WGL is desktop GL only.
  m_mode = Mode::OpenGL;

  if (core)
  {
    if (!wglMakeCurrent(m_hdc, m_hglrc))
    {
      PanicAlertFmt("(5) Can't make dummy WGL context current.");
      return false;
    }

    FetchEntryPoints();
    const HGLRC core_rc = TryCoreProfiles(m_hdc, nullptr);

    // The legacy context is released whatever happened above; callers expect it not to be
    // current when the core attempt failed.
    if (!wglMakeCurrent(m_hdc, nullptr))
    {
      PanicAlertFmt("(6) Failed to switch out temporary context");
      return false;
    }

    if (core_rc == nullptr)
    {
      WARN_LOG_FMT(VIDEO, "Failed to create a core OpenGL context. Using fallback context.");
    }
    else
    {
      wglDeleteContext(m_hglrc);
      m_hglrc = core_rc;
    }
  }

  return MakeCurrent();
}

std::unique_ptr<GLContext> GLContextWGL::CreateSharedContext()
{
  HANDLE pbuffer = nullptr;
  HDC pbuffer_dc = nullptr;
  if (!MakePbuffer(m_hdc, 1, 1, &pbuffer, &pbuffer_dc))
    return nullptr;

  const HGLRC rc = TryCoreProfiles(pbuffer_dc, m_hglrc);
  if (rc == nullptr)
  {
    s_wgl.release_pbuffer_dc(AsPbuffer(pbuffer), pbuffer_dc);
    s_wgl.destroy_pbuffer(AsPbuffer(pbuffer));
    return nullptr;
  }

  auto shared = std::make_unique<GLContextWGL>();
  shared->m_pbuffer = pbuffer;
  shared->m_hdc = pbuffer_dc;
  shared->m_hglrc = rc;
  shared->m_mode = m_mode;
  shared->m_shared = true;
  return shared;
}

HGLRC GLContextWGL::TryCoreProfiles(HDC dc, HGLRC share_with)
{
  if (s_wgl.create_context_attribs == nullptr)
  {
    WARN_LOG_FMT(VIDEO, "Missing WGL_ARB_create_context extension");
    return nullptr;
  }

  for (const GLVersion& want : CORE_VERSIONS_NEWEST_FIRST)
  {
    const std::array<int, 10> attribs{
        WGL_TOKEN_CONTEXT_PROFILE_MASK,  WGL_PROFILE_CORE,
        WGL_TOKEN_CONTEXT_FLAGS,         CORE_CONTEXT_FLAGS,
        WGL_TOKEN_CONTEXT_MAJOR_VERSION, want.major,
        WGL_TOKEN_CONTEXT_MINOR_VERSION, want.minor,
        0,                               0,
    };
    const HGLRC rc = s_wgl.create_context_attribs(dc, share_with, attribs.data());
    if (rc != nullptr)
    {
      INFO_LOG_FMT(VIDEO, "WGL: Created a GL {}.{} core context", want.major, want.minor);
      return rc;
    }
  }

  WARN_LOG_FMT(VIDEO, "Unable to create a core OpenGL context of an acceptable version");
  return nullptr;
}

bool GLContextWGL::MakeCurrent()
{
  return wglMakeCurrent(m_hdc, m_hglrc) == TRUE;
}

bool GLContextWGL::ClearCurrent()
{
  return wglMakeCurrent(m_hdc, nullptr) == TRUE;
}

void GLContextWGL::Update()
{
  ReadClientSize();
}
