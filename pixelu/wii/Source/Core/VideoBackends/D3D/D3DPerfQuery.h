// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoCommon/PerfQueryBase.h"

namespace DX11
{
// PE pixel counters from D3D11 occlusion queries kept in a ring: queries are
// begun at the write end and their results collected from the read end.
class PerfQuery : public PerfQueryBase
{
public:
  PerfQuery();
  ~PerfQuery() override;

  void EnableQuery(PerfQueryGroup group) override;
  void DisableQuery(PerfQueryGroup group) override;
  void ResetQuery() override;
  u32 GetQueryResult(PerfQueryType type) override;
  void FlushResults() override;
  bool IsFlushed() const override;

private:
  struct RingEntry
  {
    ComPtr<ID3D11Query> query;
    PerfQueryGroup group{};
  };

  // Collects results that are already available, without waiting.
  void CollectReady();
  // Waits for the oldest query and adds its result. The ring must not be empty.
  void CollectOldest();

  RingEntry& EntryAt(u32 offset_from_read);
  // Scales a sample count from the EFB's resolution to the native 640x528.
  static u64 ToNativeResolution(u64 samples);

  // 64 entries proved too few in Super Mario Sunshine; 128 were enough.
  static const int PERF_QUERY_BUFFER_SIZE = 512;

  std::array<RingEntry, PERF_QUERY_BUFFER_SIZE> m_ring;
  u32 m_read_index = 0;
};

}  // namespace DX11
