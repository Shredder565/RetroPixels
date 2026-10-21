// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/D3D12PerfQuery.h"

#include <algorithm>
#include <cstring>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/D3D12Gfx.h"
#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace DX12
{
namespace
{
bool IsCountedGroup(PerfQueryGroup group)
{
  return group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP;
}

// Sample counts are reported as if the EFB had its native size and one
// sample per pixel.
u32 ScaleToNative(u64 samples)
{
  u64 scaled = samples * EFB_WIDTH / g_framebuffer_manager->GetEFBWidth() * EFB_HEIGHT /
               g_framebuffer_manager->GetEFBHeight();
  if (g_ActiveConfig.iMultisamples > 1)
    scaled /= static_cast<u64>(g_ActiveConfig.iMultisamples);
  return static_cast<u32>(scaled);
}
}  // namespace

PerfQuery::PerfQuery() = default;

PerfQuery::~PerfQuery() = default;

bool PerfQuery::Initialize()
{
  ID3D12Device* const device = g_dx_context->GetDevice();

  D3D12_QUERY_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = PERF_QUERY_BUFFER_SIZE;
  HRESULT hr = device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&m_query_heap));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create query heap: {}", DX12HRWrap(hr));
  if (FAILED(hr))
    return false;

  D3D12_HEAP_PROPERTIES readback = {};
  readback.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer = {};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Alignment = 0;
  buffer.Width = PERF_QUERY_BUFFER_SIZE * sizeof(PerfQueryDataType);
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.Format = DXGI_FORMAT_UNKNOWN;
  buffer.SampleDesc.Count = 1;
  buffer.SampleDesc.Quality = 0;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
  hr = device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_PPV_ARGS(&m_query_readback_buffer));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create query buffer: {}", DX12HRWrap(hr));
  return SUCCEEDED(hr);
}

void PerfQuery::EnableQuery(PerfQueryGroup group)
{
  // Past half full, collect what has finished (resolving first if half the
  // ring is unresolved); a completely full ring has to wait.
  const u32 in_flight = m_query_count.load(std::memory_order_relaxed);
  constexpr u32 HALF = PERF_QUERY_BUFFER_SIZE / 2;
  if (in_flight > HALF)
    PartialFlush(m_unresolved_queries > HALF, in_flight == PERF_QUERY_BUFFER_SIZE);

  // A query may not stay open across a submission, and drawing can submit
  // (when descriptors run out), so all state is applied before it begins.
  // The caller has bound everything the draw needs by now.
  Gfx::GetInstance()->ApplyState();

  if (!IsCountedGroup(group))
    return;

  ActiveQuery& slot = m_query_buffer[m_query_next_pos];
  ASSERT(!slot.has_value && !slot.resolved);
  slot.has_value = true;
  slot.query_group = group;
  g_dx_context->GetCommandList()->BeginQuery(m_query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                             m_query_next_pos);
}

void PerfQuery::DisableQuery(PerfQueryGroup group)
{
  if (!IsCountedGroup(group))
    return;

  g_dx_context->GetCommandList()->EndQuery(m_query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                           m_query_next_pos);
  m_query_next_pos = (m_query_next_pos + 1) % PERF_QUERY_BUFFER_SIZE;
  m_query_count.fetch_add(1, std::memory_order_relaxed);
  ++m_unresolved_queries;
}

void PerfQuery::ResetQuery()
{
  m_query_count.store(0, std::memory_order_relaxed);
  m_unresolved_queries = 0;
  m_query_resolve_pos = 0;
  m_query_readback_pos = 0;
  m_query_next_pos = 0;
  for (auto& total : m_results)
    total.store(0, std::memory_order_relaxed);
  for (ActiveQuery& slot : m_query_buffer)
  {
    slot.fence_value = 0;
    slot.resolved = false;
    slot.has_value = false;
  }
}

u32 PerfQuery::GetQueryResult(PerfQueryType type)
{
  const auto total = [this](PerfQueryGroup group) {
    return m_results[group].load(std::memory_order_relaxed);
  };

  u32 value;
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
    value = 0;
    break;
  }

  // The counters run at four times the reported rate.
  return value / 4;
}

void PerfQuery::FlushResults()
{
  while (!IsFlushed())
    PartialFlush(true, true);
}

bool PerfQuery::IsFlushed() const
{
  return m_query_count.load(std::memory_order_relaxed) == 0;
}

void PerfQuery::ResolveQueries()
{
  // A run that wraps past the end of the ring is resolved in two parts.
  const u32 to_end = PERF_QUERY_BUFFER_SIZE - m_query_resolve_pos;
  if (m_unresolved_queries > to_end)
    ResolveQueries(to_end);
  ResolveQueries(m_unresolved_queries);
}

void PerfQuery::ResolveQueries(u32 query_count)
{
  DEBUG_ASSERT(m_unresolved_queries >= query_count &&
               (m_query_resolve_pos + query_count) <= PERF_QUERY_BUFFER_SIZE);

  g_dx_context->GetCommandList()->ResolveQueryData(
      m_query_heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, m_query_resolve_pos, query_count,
      m_query_readback_buffer.Get(), m_query_resolve_pos * sizeof(PerfQueryDataType));

  // The values become readable once the list being recorded has completed.
  const u64 ready_at = g_dx_context->GetCurrentFenceValue();
  for (u32 n = 0; n < query_count; ++n)
  {
    ActiveQuery& slot = m_query_buffer[m_query_resolve_pos + n];
    DEBUG_ASSERT(slot.has_value && !slot.resolved);
    slot.fence_value = ready_at;
    slot.resolved = true;
  }
  m_query_resolve_pos = (m_query_resolve_pos + query_count) % PERF_QUERY_BUFFER_SIZE;
  m_unresolved_queries -= query_count;
}

void PerfQuery::ReadbackQueries(bool blocking)
{
  u64 completed = g_dx_context->GetCompletedFenceValue();

  // Collect a contiguous run of finished queries from the read position. The
  // run is taken in two pieces when it reaches the end of the ring.
  const u32 outstanding = m_query_count.load(std::memory_order_relaxed);
  u32 run = 0;
  for (u32 n = 0; n < outstanding; ++n)
  {
    const ActiveQuery& slot = m_query_buffer[(m_query_readback_pos + run) % PERF_QUERY_BUFFER_SIZE];
    if (!slot.resolved)
      break;

    if (slot.fence_value > completed)
    {
      if (!blocking)
        break;

      // Resolved queries always belong to a submitted list.
      ASSERT(slot.fence_value != g_dx_context->GetCurrentFenceValue());
      g_dx_context->WaitForFence(slot.fence_value);
      completed = g_dx_context->GetCompletedFenceValue();
    }

    if (m_query_readback_pos + run >= PERF_QUERY_BUFFER_SIZE)
    {
      AccumulateQueriesFromBuffer(run);
      DEBUG_ASSERT(m_query_readback_pos == 0);
      run = 0;
    }
    ++run;
  }

  if (run != 0)
    AccumulateQueriesFromBuffer(run);
}

void PerfQuery::AccumulateQueriesFromBuffer(u32 query_count)
{
  ASSERT(query_count <= m_query_count.load(std::memory_order_relaxed) &&
         (m_query_readback_pos + query_count) <= PERF_QUERY_BUFFER_SIZE);

  constexpr size_t STRIDE = sizeof(PerfQueryDataType);
  D3D12_RANGE read = {};
  read.Begin = m_query_readback_pos * STRIDE;
  read.End = (m_query_readback_pos + query_count) * STRIDE;
  u8* mapped;
  const HRESULT hr = m_query_readback_buffer->Map(0, &read, reinterpret_cast<void**>(&mapped));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to map query readback buffer: {}", DX12HRWrap(hr));
  if (FAILED(hr))
    return;

  for (u32 n = 0; n < query_count; ++n)
  {
    const u32 index = (m_query_readback_pos + n) % PERF_QUERY_BUFFER_SIZE;
    ActiveQuery& slot = m_query_buffer[index];
    ASSERT(slot.fence_value != 0);
    slot.fence_value = 0;
    slot.resolved = false;
    slot.has_value = false;

    PerfQueryDataType samples;
    std::memcpy(&samples, mapped + index * STRIDE, sizeof(samples));
    m_results[slot.query_group].fetch_add(ScaleToNative(samples), std::memory_order_relaxed);
  }

  const D3D12_RANGE nothing_written = {0, 0};
  m_query_readback_buffer->Unmap(0, &nothing_written);

  m_query_readback_pos = (m_query_readback_pos + query_count) % PERF_QUERY_BUFFER_SIZE;
  m_query_count.fetch_sub(query_count, std::memory_order_relaxed);
}

void PerfQuery::PartialFlush(bool resolve, bool blocking)
{
  // Submitting is what writes unresolved queries into the readback buffer.
  if (resolve && m_unresolved_queries > 0)
    Gfx::GetInstance()->ExecuteCommandList(false);

  ReadbackQueries(blocking);
}
}  // namespace DX12
