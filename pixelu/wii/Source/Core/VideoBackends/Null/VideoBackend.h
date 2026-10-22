// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "VideoCommon/VideoBackendBase.h"

namespace Null
{
// A video backend that renders nothing. All of the GX emulation shared with the other backends
// still runs in VideoCommon, which makes it useful for testing and benchmarking the rest.
class VideoBackend final : public VideoBackendBase
{
public:
  bool Initialize(const WindowSystemInfo& wsi) override;
  void Shutdown() override;

  std::string GetConfigName() const override { return CONFIG_NAME; }
  std::string GetDisplayName() const override;
  void InitBackendInfo(const WindowSystemInfo& wsi) override;

  static constexpr const char* CONFIG_NAME = "Null";
};
}  // namespace Null
