// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>

#include "Common/GL/GLContext.h"

namespace GLEGL
{
struct Connection;
}

// OpenGL or OpenGL ES through EGL (libEGL.so.1, opened at run time), for three window systems:
//  - X11: like GLX, a private connection to the frontend's server and a child window of its
//    render window (GLX11::Target);
//  - Wayland: a wl_egl_window on the frontend's wl_surface (libwayland-egl.so.1);
//  - headless: no window at all (Mesa's surfaceless platform where offered, else the default
//    display), for programs without a user interface.
// Desktop GL is tried first unless OpenGL ES was asked for; each falls back to the other.
// Shared contexts are surfaceless where EGL_KHR_surfaceless_context allows, else they get a
// 1x1 pbuffer.
class GLContextEGL final : public GLContext
{
public:
  ~GLContextEGL() override;

  bool IsHeadless() const override;

  std::unique_ptr<GLContext> CreateSharedContext() override;

  bool MakeCurrent() override;
  bool ClearCurrent() override;

  void Update() override;
  void SetSurfaceSize(u32 width, u32 height) override;

  void Swap() override;
  void SwapInterval(int interval) override;

  void* GetFuncAddress(const std::string& name) override;

protected:
  bool Initialize(const WindowSystemInfo& wsi, bool stereo, bool core) override;

private:
  // Picks a config and makes a context for one client API (OpenGL or OpenGL ES).
  bool CreateContextFor(Mode api, bool core);
  bool CreateWindowSurface();

  std::shared_ptr<GLEGL::Connection> m_connection;
  void* m_config = nullptr;          // EGLConfig
  void* m_context = nullptr;         // EGLContext
  void* m_surface = nullptr;         // EGLSurface (EGL_NO_SURFACE when surfaceless)
  void* m_wayland_window = nullptr;  // wl_egl_window
  bool m_core_profile = false;
  int m_version_major = 0;  // 0: a context made without a version (legacy desktop GL)
  int m_version_minor = 0;
  bool m_has_size_from_frontend = false;
};
