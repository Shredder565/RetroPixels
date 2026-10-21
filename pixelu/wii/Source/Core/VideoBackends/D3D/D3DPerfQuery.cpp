// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/D3DPerfQuery.h"

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
namespace
{
bool IsCountedGroup(PerfQueryGroup group)
{
  // Only the z-compare groups are backed by occlusion queries.
  return group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP;
}
}  // namespace

PerfQuery::PerfQuery()
{
  D3D11_QUERY_DESC desc;
  desc.Query = D3D11_QUERY_OCCLUSION;
  desc.MiscFlags = 0;
  for (RingEntry& entry : m_ring)
    D3D::device->CreateQuery(&desc, &entry.query);

  ResetQuery();
}

PerfQuery::~PerfQuery() = default;

PerfQuery::RingEntry& PerfQuery::EntryAt(u32 offset_from_read)
{
  return m_ring[(m_read_index + offset_from_read) % m_ring.size()];
}

u64 PerfQuery::ToNativeResolution(u64 samples)
{
  // Width first, then height, each rounding down, as the counters always have.
  return samples * EFB_WIDTH / g_framebuffer_manager->GetEFBWidth() * EFB_HEIGHT /
         g_framebuffer_manager->GetEFBHeight();
}

void PerfQuery::EnableQuery(PerfQueryGroup group)
{
  u32 pending = m_query_count.load(std::memory_order_relaxed);

  // Past half full, pick up whatever has finished.
  if (pending > m_ring.size() / 2)
  {
    CollectReady();
    pending = m_query_count.load(std::memory_order_relaxed);
  }

  // Still full: wait for the oldest one to make room.
  if (pending == m_ring.size())
  {
    CollectOldest();
    pending = m_query_count.load(std::memory_order_relaxed);
    ERROR_LOG_FMT(VIDEO, "Flushed query buffer early!");
  }

  if (!IsCountedGroup(group))
    return;

  RingEntry& entry = EntryAt(pending);
  D3D::context->Begin(entry.query.Get());
  entry.group = group;
  m_query_count.fetch_add(1, std::memory_order_relaxed);
}

void PerfQuery::DisableQuery(PerfQueryGroup group)
{
  if (!IsCountedGroup(group))
    return;

  // The newest entry is the one EnableQuery began.
  const u32 newest =
      static_cast<u32>(m_query_count.load(std::memory_order_relaxed) + m_ring.size() - 1);
  D3D::context->End(EntryAt(newest).query.Get());
}

void PerfQuery::ResetQuery()
{
  m_query_count.store(0, std::memory_order_relaxed);
  for (std::atomic<u32>& total : m_results)
    total.store(0, std::memory_order_relaxed);
}

u32 PerfQuery::GetQueryResult(PerfQueryType type)
{
  const auto total = [this](PerfQueryGroup group) {
    return m_results[group].load(std::memory_order_relaxed);
  };

  u32 value = 0;
  switch (type)
  {
  case PQ_ZCOMP_INPUT_ZCOMPLOC:
  case PQ_ZCOMP_OUTPUT_ZCOMPLOC:
    value = total(PQG_ZCOMP_ZCOMPLOC);
    break;
  case PQ_ZCOMP_INPUT:
  case PQ_ZCOMP_OUTPUT:
    value = total(PQG_ZCOMP);
    break;
  case PQ_BLEND_INPUT:
    value = total(PQG_ZCOMP) + total(PQG_ZCOMP_ZCOMPLOC);
    break;
  case PQ_EFB_COPY_CLOCKS:
    value = total(PQG_EFB_COPY_CLOCKS);
    break;
  default:
    break;
  }

  // The counters count in quads of pixels.
  return value / 4;
}

void PerfQuery::CollectOldest()
{
  RingEntry& oldest = m_ring[m_read_index];

  // Spins until the GPU has the result.
  UINT64 samples = 0;
  HRESULT status = S_FALSE;
  while (status != S_OK)
    status = D3D::context->GetData(oldest.query.Get(), &samples, sizeof(samples), 0);

  // Counts are kept relative to native resolution and single sampling.
  u64 native = ToNativeResolution(samples);
  if (g_ActiveConfig.iMultisamples > 1)
    native /= g_ActiveConfig.iMultisamples;
  m_results[oldest.group].fetch_add(static_cast<u32>(native), std::memory_order_relaxed);

  m_read_index = static_cast<u32>((m_read_index + 1) % m_ring.size());
  m_query_count.fetch_sub(1, std::memory_order_relaxed);
}

void PerfQuery::FlushResults()
{
  while (!IsFlushed())
    CollectOldest();
}

void PerfQuery::CollectReady()
{
  while (!IsFlushed())
  {
    RingEntry& oldest = m_ring[m_read_index];
    UINT64 samples = 0;
    const HRESULT status = D3D::context->GetData(oldest.query.Get(), &samples, sizeof(samples),
                                                 D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (status != S_OK)
      return;

    // This path replaces the group's total rather than adding to it, and
    // does not divide by the sample count.
    m_results[oldest.group].store(static_cast<u32>(ToNativeResolution(samples)),
                                  std::memory_order_relaxed);
    m_read_index = static_cast<u32>((m_read_index + 1) % m_ring.size());
    m_query_count.fetch_sub(1, std::memory_order_relaxed);
  }
}

bool PerfQuery::IsFlushed() const
{
  return m_query_count.load(std::memory_order_relaxed) == 0;
}

}  // namespace DX11
