// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include "Common/GL/GLContext.h"

// OpenGL through WGL (opengl32.dll). The main context draws to the render window's device
// context; shared contexts for worker threads draw to a 1x1 pbuffer, since WGL has no
// surfaceless contexts.
class GLContextWGL final : public GLContext
{
public:
  ~GLContextWGL() override;

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
  // Tries the core-profile versions newest first; nullptr when none could be made.
  static HGLRC TryCoreProfiles(HDC dc, HGLRC share_with);

  bool ReadClientSize();
  void ReleaseDrawable();

  HWND m_hwnd = nullptr;
  HANDLE m_pbuffer = nullptr;
  HDC m_hdc = nullptr;
  HGLRC m_hglrc = nullptr;
};
