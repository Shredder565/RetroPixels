// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/BoundingBox.h"

#include <algorithm>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VideoConfig.h"

std::unique_ptr<BoundingBox> g_bounding_box;

bool BoundingBox::UseFallback()
{
  return !(g_ActiveConfig.bBBoxEnable && g_backend_info.bSupportsBBox);
}

void BoundingBox::Enable(PixelShaderManager& pixel_shader_manager)
{
  m_active = true;
  pixel_shader_manager.SetBoundingBoxActive(true);
}

void BoundingBox::Disable(PixelShaderManager& pixel_shader_manager)
{
  m_active = false;
  pixel_shader_manager.SetBoundingBoxActive(false);
}

void BoundingBox::Flush()
{
  if (UseFallback())
    return;

  // Whatever is drawn next may move the box, so the cached copy can no longer be trusted.
  m_cache_valid = false;

  // Send each run of consecutive pending values in a single backend write.
  u32 index = 0;
  while (index < NUM_BBOX_VALUES)
  {
    if (!m_pending_write[index])
    {
      ++index;
      continue;
    }
    const u32 first = index;
    while (index < NUM_BBOX_VALUES && m_pending_write[index])
      m_pending_write[index++] = false;
    Write(first, std::span<const BBoxType>(m_cache.data() + first, index - first));
  }
}

void BoundingBox::RefreshFromBackend()
{
  if (!g_backend_info.bSupportsBBox)
    return;

  const std::vector<BBoxType> fresh = Read(0, NUM_BBOX_VALUES);
  // Values the game wrote since the last flush win over what the GPU holds.
  for (u32 i = 0; i < NUM_BBOX_VALUES; ++i)
    m_cache[i] = m_pending_write[i] ? m_cache[i] : fresh[i];

  m_cache_valid = true;
}

u16 BoundingBox::Get(u32 index)
{
  ASSERT(index < NUM_BBOX_VALUES);

  if (UseFallback())
    return m_fallback[index];

  if (!m_cache_valid)
    RefreshFromBackend();
  return static_cast<u16>(m_cache[index]);
}

void BoundingBox::Set(u32 index, u16 value)
{
  ASSERT(index < NUM_BBOX_VALUES);

  if (UseFallback())
  {
    m_fallback[index] = value;
    return;
  }

  // Skip redundant writes when the cache is known to match the GPU.
  const bool unchanged = m_cache_valid && m_cache[index] == value;
  if (unchanged)
    return;
  m_cache[index] = value;
  m_pending_write[index] = true;
}

// Savestates taken mid-draw may not capture the box exactly; the layout below at least keeps
// the CPU cache and the GPU copy separate so neither disturbs the other.
void BoundingBox::DoState(PointerWrap& p)
{
  p.DoArray(m_fallback);
  p.Do(m_active);
  p.DoArray(m_cache);
  p.DoArray(m_pending_write);
  p.Do(m_cache_valid);

  // The GPU-side values travel as their own vector, read or written directly so that the
  // cache state above is left untouched.
  const bool gpu_side = g_backend_info.bSupportsBBox;
  std::vector<BBoxType> gpu_values(NUM_BBOX_VALUES);
  if (!p.IsReadMode())
  {
    if (gpu_side)
      gpu_values = Read(0, NUM_BBOX_VALUES);
    p.Do(gpu_values);
    return;
  }

  p.Do(gpu_values);
  if (gpu_side)
    Write(0, gpu_values);
}
