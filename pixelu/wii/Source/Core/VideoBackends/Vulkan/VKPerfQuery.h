// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoCommon/PerfQueryBase.h"

namespace Vulkan
{
// Occlusion queries in a ring over one query pool. Each query remembers the submission it was
// recorded in; results are collected once that submission has completed.
class PerfQuery : public PerfQueryBase
{
public:
  PerfQuery();
  ~PerfQuery() override;

  static PerfQuery* GetInstance() { return static_cast<PerfQuery*>(g_perf_query.get()); }

  bool Initialize() override;

  void EnableQuery(PerfQueryGroup group) override;
  void DisableQuery(PerfQueryGroup group) override;
  void ResetQuery() override;
  u32 GetQueryResult(PerfQueryType type) override;
  void FlushResults() override;
  bool IsFlushed() const override;

private:
  // Large enough for the games tested (64 was not, 128 was).
  static constexpr u32 RING_SIZE = 512;

  struct Slot
  {
    u64 fence_counter;
    PerfQueryGroup group;
    bool in_use;
  };

  // Collects every finished query from the front of the ring.
  void CollectFinished();
  // Reads `count` results starting at the front (no wrap) and frees their slots.
  void Collect(u32 count);
  // Submits if needed (always when blocking, which also waits), then collects.
  void PartialFlush(bool blocking);

  VkQueryPool m_pool = VK_NULL_HANDLE;
  u32 m_front = 0;
  u32 m_back = 0;
  std::array<Slot, RING_SIZE> m_slots = {};
  std::array<u32, RING_SIZE> m_samples = {};
};

}  // namespace Vulkan
