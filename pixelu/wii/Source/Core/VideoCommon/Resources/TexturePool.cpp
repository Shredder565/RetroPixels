// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Resources/TexturePool.h"

#include "Common/Logging/Log.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"

namespace VideoCommon
{
void TexturePool::Reset()
{
  m_free.clear();
}

std::unique_ptr<AbstractTexture> TexturePool::AllocateTexture(const TextureConfig& config)
{
  if (const auto pooled = m_free.find(config); pooled != m_free.end())
  {
    std::unique_ptr<AbstractTexture> reused = std::move(pooled->second);
    m_free.erase(pooled);
    return reused;
  }

  std::unique_ptr<AbstractTexture> created = g_gfx->CreateTexture(config);
  if (created == nullptr)
  {
    ERROR_LOG_FMT(VIDEO, "Failed to allocate a {}x{}x{} texture", config.width, config.height,
                  config.layers);
  }
  return created;
}

void TexturePool::ReleaseTexture(std::unique_ptr<AbstractTexture> texture)
{
  const TextureConfig config = texture->GetConfig();
  m_free.emplace(config, std::move(texture));
}
}  // namespace VideoCommon
