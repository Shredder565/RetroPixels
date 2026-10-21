// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>

#include "Common/GL/GLContext.h"

namespace GLX11
{
class Target;
}

// Desktop OpenGL on X11 through GLX (libGL.so.1, opened at run time). The main context draws to
// a child window of the frontend's render window (see GLX11::Target); shared contexts for worker
// threads draw to a 1x1 pbuffer, or to no drawable at all where the pbuffer cannot be made.
class GLContextGLX final : public GLContext
{
public:
  ~GLContextGLX() override;

  bool IsHeadless() const override;

  std::unique_ptr<GLContext> CreateSharedContext() override;

  bool MakeCurrent() override;
  bool ClearCurrent() override;

  void Update() override;

  void Swap() override;
  void SwapInterval(int interval) override;

  void* GetFuncAddress(const std::string& name) override;

protected:
  bool Initialize(const WindowSystemInfo& wsi, bool stereo, bool core) override;

private:
  // Makes a context on m_config sharing with `share` (may be null); core profiles newest first,
  // then (if `allow_legacy`) a context of whatever version the driver gives. nullptr on failure.
  void* MakeContext(void* config, void* share, bool core, bool allow_legacy);

  std::shared_ptr<GLX11::Target> m_target;
  void* m_config = nullptr;      // GLXFBConfig
  void* m_context = nullptr;     // GLXContext
  unsigned long m_drawable = 0;  // GLXWindow or GLXPbuffer
  bool m_drawable_is_pbuffer = false;
  bool m_core_profile = false;
};
