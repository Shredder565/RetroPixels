// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <unordered_map>

#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/TextureConfig.h"

namespace VideoCommon
{
// Keeps released textures so a later request with the same configuration can reuse one
// instead of creating a new texture.
class TexturePool
{
public:
  void Reset();

  // A pooled texture with exactly this configuration, or a newly created one (null on failure).
  std::unique_ptr<AbstractTexture> AllocateTexture(const TextureConfig& config);
  void ReleaseTexture(std::unique_ptr<AbstractTexture> texture);

private:
  std::unordered_multimap<TextureConfig, std::unique_ptr<AbstractTexture>> m_free;
};
}  // namespace VideoCommon
