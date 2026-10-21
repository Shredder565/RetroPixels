// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Common.h"
#include "VideoCommon/VideoBackendBase.h"

namespace Vulkan
{
// The Vulkan video backend as seen by VideoCommon's backend list.
class VideoBackend : public VideoBackendBase
{
public:
  static constexpr const char* CONFIG_NAME = "Vulkan";

  bool Initialize(const WindowSystemInfo& wsi) override;
  void Shutdown() override;

  std::string GetConfigName() const override { return CONFIG_NAME; }
  std::string GetDisplayName() const override { return _trans("Vulkan"); }

  void InitBackendInfo(const WindowSystemInfo& wsi) override;
  void PrepareWindow(WindowSystemInfo& wsi) override;
};
}  // namespace Vulkan
