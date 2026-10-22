// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

namespace VideoCommon
{
// Receives notifications about an asset that something depends on.
class AssetListener
{
public:
  AssetListener() = default;
  virtual ~AssetListener() = default;

  AssetListener(const AssetListener&) = default;
  AssetListener(AssetListener&&) = default;
  AssetListener& operator=(const AssetListener&) = default;
  AssetListener& operator=(AssetListener&&) = default;

  // The asset finished loading and its data can be used.
  virtual void NotifyAssetLoadSuccess() = 0;
  // Loading the asset was attempted and did not succeed.
  virtual void NotifyAssetLoadFailed() = 0;
  // The asset's data was released.
  virtual void AssetUnloaded() = 0;
};
}  // namespace VideoCommon
