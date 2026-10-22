// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "VideoCommon/VideoBackendBase.h"

namespace DX12
{
// Registers the Direct3D 12 renderer with the video core.
class VideoBackend final : public VideoBackendBase
{
public:
  bool Initialize(const WindowSystemInfo& wsi) override;
  void Shutdown() override;

  std::string GetConfigName() const override;
  std::string GetDisplayName() const override;
  void InitBackendInfo(const WindowSystemInfo& wsi) override;

  static constexpr const char* CONFIG_NAME = "D3D12";

private:
  void FillBackendInfo();
};
}  // namespace DX12
