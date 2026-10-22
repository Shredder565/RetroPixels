// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/D3D12StreamBuffer.h"

#include <algorithm>
#include <functional>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/D3D12/DX12Context.h"

namespace DX12
{
StreamBuffer::StreamBuffer() = default;

StreamBuffer::~StreamBuffer()
{
  if (m_host_pointer)
  {
    const D3D12_RANGE written = {0, m_size};
    m_buffer->Unmap(0, &written);
  }

  // Only destroyed at shutdown, when the GPU is idle, so released directly.
  if (m_buffer)
    m_buffer->Release();
}

bool StreamBuffer::AllocateBuffer(u32 size)
{
  D3D12_HEAP_PROPERTIES upload_heap = {};
  upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Alignment = 0;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc = {1, 0};
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  HRESULT result = g_dx_context->GetDevice()->CreateCommittedResource(
      &upload_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
      IID_PPV_ARGS(&m_buffer));
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to allocate buffer of size {}: {}", size,
             DX12HRWrap(result));
  if (FAILED(result))
    return false;

  // The CPU never reads it back.
  static const D3D12_RANGE nothing_read = {};
  result = m_buffer->Map(0, &nothing_read, reinterpret_cast<void**>(&m_host_pointer));
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to map buffer of size {}: {}", size,
             DX12HRWrap(result));
  if (FAILED(result))
    return false;

  // Start empty, with nothing in flight.
  m_tracked_fences.clear();
  m_current_offset = 0;
  m_current_gpu_position = 0;
  m_size = size;
  m_gpu_pointer = m_buffer->GetGPUVirtualAddress();
  return true;
}

bool StreamBuffer::ReserveMemory(u32 num_bytes, u32 alignment)
{
  // Enough for the data at any alignment.
  const u32 needed = num_bytes + alignment;
  if (needed > m_size)
  {
    PanicAlertFmt("Attempting to allocate {} bytes from a {} byte stream buffer", num_bytes,
                  m_size);
    return false;
  }

  UpdateCurrentFencePosition();

  // The write position may never land exactly on the GPU position: equal
  // positions mean "GPU caught up", so the comparisons against the GPU
  // position are strict.
  std::optional<u32> place;
  if (m_current_offset >= m_current_gpu_position)
  {
    // Ahead of the GPU: use the tail, or wrap to the start if the GPU is far enough on.
    if (needed <= m_size - m_current_offset)
      place = Common::AlignUp(m_current_offset, alignment);
    else if (needed < m_current_gpu_position)
      place = 0;
  }
  else if (needed < m_current_gpu_position - m_current_offset)
  {
    // Behind the GPU: use the gap up to it.
    place = Common::AlignUp(m_current_offset, alignment);
  }

  // Otherwise wait for a submitted command list to finish with enough space.
  if (!place && WaitForClearSpace(needed))
    place = Common::AlignUp(m_current_offset, alignment);

  if (!place)
    return false;

  m_current_offset = *place;
  m_last_allocation_size = num_bytes;
  return true;
}

void StreamBuffer::CommitMemory(u32 final_num_bytes)
{
  ASSERT((m_current_offset + final_num_bytes) <= m_size);
  ASSERT(final_num_bytes <= m_last_allocation_size);
  m_current_offset += final_num_bytes;
}

void StreamBuffer::UpdateCurrentFencePosition()
{
  // Nothing new to track when the GPU is already level with us.
  if (m_current_offset == m_current_gpu_position)
    return;

  // Writes since the last submit belong to the same fence; just move its offset.
  const u64 fence = g_dx_context->GetCurrentFenceValue();
  if (!m_tracked_fences.empty() && m_tracked_fences.back().first == fence)
  {
    m_tracked_fences.back().second = m_current_offset;
    return;
  }

  UpdateGPUPosition();
  m_tracked_fences.emplace_back(fence, m_current_offset);
}

void StreamBuffer::UpdateGPUPosition()
{
  const u64 completed = g_dx_context->GetCompletedFenceValue();
  auto passed = m_tracked_fences.begin();
  for (; passed != m_tracked_fences.end() && passed->first <= completed; ++passed)
    m_current_gpu_position = passed->second;

  if (passed != m_tracked_fences.begin())
    m_tracked_fences.erase(m_tracked_fences.begin(), passed);
}

std::optional<std::pair<u32, u32>> StreamBuffer::SpaceIfGPUAt(u32 gpu_position, u32 num_bytes) const
{
  // Level with us: everything written so far is consumed, start over at zero.
  // (This is the case after a submit with nothing written since.)
  if (gpu_position == m_current_offset)
    return std::pair<u32, u32>{0, 0};

  if (gpu_position < m_current_offset)
  {
    // The GPU would be behind us: the tail is free, and so is the start up to it.
    if (m_size - m_current_offset >= num_bytes)
      return std::pair<u32, u32>{m_current_offset, gpu_position};
    if (gpu_position > num_bytes)
      return std::pair<u32, u32>{0, gpu_position};
    return std::nullopt;
  }

  // The GPU would be ahead of us: the gap in between is free.
  if (gpu_position - m_current_offset > num_bytes)
    return std::pair<u32, u32>{m_current_offset, gpu_position};
  return std::nullopt;
}

bool StreamBuffer::WaitForClearSpace(u32 num_bytes)
{
  auto fence = m_tracked_fences.begin();
  std::optional<std::pair<u32, u32>> outcome;
  for (; fence != m_tracked_fences.end(); ++fence)
  {
    outcome = SpaceIfGPUAt(fence->second, num_bytes);
    if (outcome)
      break;
  }

  // Nothing helps, or the fence that would is the one not yet submitted:
  // the caller must submit first.
  if (fence == m_tracked_fences.end() || fence->first == g_dx_context->GetCurrentFenceValue())
    return false;

  g_dx_context->WaitForFence(fence->first);

  // Forget that fence and every older one; when the GPU ends level with us,
  // nothing tracked is of any further use.
  const bool level = fence->second == m_current_offset;
  m_tracked_fences.erase(m_tracked_fences.begin(), level ? m_tracked_fences.end() : fence + 1);
  m_current_offset = outcome->first;
  m_current_gpu_position = outcome->second;
  return true;
}

}  // namespace DX12
