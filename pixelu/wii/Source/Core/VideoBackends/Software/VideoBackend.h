// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include "VideoCommon/VideoBackendBase.h"

namespace SW
{
// The software renderer: a slow, CPU-only GX implementation meant for debugging the hardware
// backends.
class VideoSoftware : public VideoBackendBase
{
  bool Initialize(const WindowSystemInfo& wsi) override;
  void Shutdown() override;

  std::string GetConfigName() const override;
  std::string GetDisplayName() const override;
  std::optional<std::string> GetWarningMessage() const override;

  void InitBackendInfo(const WindowSystemInfo& wsi) override;

  static constexpr const char* CONFIG_NAME = "Software Renderer";
};
}  // namespace SW
