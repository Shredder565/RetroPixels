// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/WindowSystemInfo.h"

// An OpenGL rendering context bound to one window (or an offscreen surface). The base class is
// the "no context" implementation: every operation fails or does nothing. A window-system
// specific subclass (WGL on Windows) does the real work.
class GLContext
{
public:
  enum class Mode
  {
    Detect,
    OpenGL,
    OpenGLES
  };

  virtual ~GLContext();

  Mode GetMode() const { return m_mode; }
  bool IsGLES() const { return m_mode == Mode::OpenGLES; }

  // Size of the drawable in pixels, as of the last Initialize() or Update().
  u32 GetBackBufferWidth() const { return m_surface_width; }
  u32 GetBackBufferHeight() const { return m_surface_height; }

  virtual bool IsHeadless() const;

  // A second context sharing objects with this one, for use on another thread.
  virtual std::unique_ptr<GLContext> CreateSharedContext();

  virtual bool MakeCurrent();
  virtual bool ClearCurrent();

  // Re-reads the drawable size after the window was resized.
  virtual void Update();
  virtual void UpdateSurface(void* window_handle);

  virtual void Swap();
  virtual void SwapInterval(int interval);

  virtual void* GetFuncAddress(const std::string& name);

  // Makes a context for the window system named in wsi. On success the new context is current
  // on the calling thread. prefer_gles asks for OpenGL ES where the platform can choose.
  static std::unique_ptr<GLContext> Create(const WindowSystemInfo& wsi, bool stereo = false,
                                           bool core = true, bool prefer_gles = false);

protected:
  struct GLVersion
  {
    int major;
    int minor;
  };

  // Desktop core-profile versions to ask for, newest first. 3.2 is the floor: the backend
  // needs geometry shaders, which core profiles only have from 3.2 on.
  static constexpr std::array<GLVersion, 9> CORE_VERSIONS_NEWEST_FIRST{{
      {4, 6},
      {4, 5},
      {4, 4},
      {4, 3},
      {4, 2},
      {4, 1},
      {4, 0},
      {3, 3},
      {3, 2},
  }};

  virtual bool Initialize(const WindowSystemInfo& wsi, bool stereo, bool core);

  Mode m_mode = Mode::Detect;
  u32 m_surface_width = 0;
  u32 m_surface_height = 0;
  bool m_shared = false;
};
