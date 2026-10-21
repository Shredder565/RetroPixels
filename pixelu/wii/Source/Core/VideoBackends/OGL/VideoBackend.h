// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include "VideoCommon/VideoBackendBase.h"

class GLContext;

namespace OGL
{
// The OpenGL / OpenGL ES video backend.
class VideoBackend : public VideoBackendBase
{
public:
  bool Initialize(const WindowSystemInfo& wsi) override;
  void Shutdown() override;

  std::string GetConfigName() const override;
  std::string GetDisplayName() const override;

  void InitBackendInfo(const WindowSystemInfo& wsi) override;

  static constexpr const char* CONFIG_NAME = "OGL";

private:
  // Loads entry points and checks for GL 3. Reports the problem to the user on failure.
  bool InitializeGLExtensions(GLContext* context);
  // Fills g_backend_info and g_ogl_config from the context's capabilities.
  bool FillBackendInfo(GLContext* context);
};
}  // namespace OGL
