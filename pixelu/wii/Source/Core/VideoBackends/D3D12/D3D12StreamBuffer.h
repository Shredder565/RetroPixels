// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <deque>
#include <optional>
#include <utility>

#include "Common/CommonTypes.h"
#include "VideoBackends/D3D12/Common.h"

namespace DX12
{
// A persistently mapped upload-heap buffer used as a ring. The CPU writes at
// the current offset; each submitted command list's fence records how far the
// GPU will have read once it completes.
class StreamBuffer
{
public:
  StreamBuffer();
  ~StreamBuffer();

  bool AllocateBuffer(u32 size);

  ID3D12Resource* GetBuffer() const { return m_buffer; }
  D3D12_GPU_VIRTUAL_ADDRESS GetGPUPointer() const { return m_gpu_pointer; }
  u8* GetHostPointer() const { return m_host_pointer; }
  u8* GetCurrentHostPointer() const { return m_host_pointer + m_current_offset; }
  D3D12_GPU_VIRTUAL_ADDRESS GetCurrentGPUPointer() const
  {
    return m_gpu_pointer + m_current_offset;
  }
  u32 GetSize() const { return m_size; }
  u32 GetCurrentOffset() const { return m_current_offset; }

  // Makes room for num_bytes at the given alignment at the current offset.
  // False means the space is held by the command list still being recorded:
  // the caller has to submit it and try again.
  bool ReserveMemory(u32 num_bytes, u32 alignment);
  // Moves past the bytes actually written (at most the reserved amount).
  void CommitMemory(u32 final_num_bytes);

private:
  // Records the current offset against the current command list's fence.
  void UpdateCurrentFencePosition();
  // Drops fences the GPU has passed, moving the GPU position forward.
  void UpdateGPUPosition();

  // Waits on the oldest fence whose completion frees num_bytes; false when no
  // submitted fence would.
  bool WaitForClearSpace(u32 num_bytes);

  // Where to continue writing, and the GPU position to assume, if the GPU
  // were at gpu_position.
  std::optional<std::pair<u32, u32>> SpaceIfGPUAt(u32 gpu_position, u32 num_bytes) const;

  u32 m_size = 0;
  u32 m_current_offset = 0;
  u32 m_current_gpu_position = 0;
  u32 m_last_allocation_size = 0;

  ID3D12Resource* m_buffer = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS m_gpu_pointer = {};
  u8* m_host_pointer = nullptr;

  // (fence value, buffer offset reached by the commands before that fence)
  std::deque<std::pair<u64, u32>> m_tracked_fences;
};

}  // namespace DX12
