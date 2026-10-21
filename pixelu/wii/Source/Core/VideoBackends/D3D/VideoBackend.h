// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include "VideoCommon/VideoBackendBase.h"

namespace DX11
{
// The Direct3D 11 video backend ("D3D" in the configuration).
class VideoBackend : public VideoBackendBase
{
public:
  bool Initialize(const WindowSystemInfo& wsi) override;
  void Shutdown() override;

  std::string GetConfigName() const override;
  std::string GetDisplayName() const override;
  std::optional<std::string> GetWarningMessage() const override;

  void InitBackendInfo(const WindowSystemInfo& wsi) override;

  static constexpr const char* CONFIG_NAME = "D3D";

private:
  void FillBackendInfo();
};
}  // namespace DX11
