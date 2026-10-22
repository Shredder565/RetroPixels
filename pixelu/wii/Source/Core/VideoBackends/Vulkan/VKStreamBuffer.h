// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <deque>
#include <memory>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/Constants.h"

namespace Vulkan
{
// A persistently mapped ring buffer the CPU writes into while the GPU reads earlier parts.
// Each submission's end position is remembered with its fence counter; once that counter has
// completed the GPU is known to be past that position and the space behind it can be reused.
class StreamBuffer
{
public:
  StreamBuffer(VkBufferUsageFlags usage, u32 size);
  ~StreamBuffer();

  VkBuffer GetBuffer() const { return m_buffer; }
  u8* GetHostPointer() const { return m_mapped; }
  u8* GetCurrentHostPointer() const { return m_mapped + m_write_pos; }
  u32 GetCurrentSize() const { return m_size; }
  u32 GetCurrentOffset() const { return m_write_pos; }

  // Finds room for num_bytes at the given alignment, waiting for the GPU if needed. False
  // means the space is held by the command buffer still being recorded: submit, then retry.
  bool ReserveMemory(u32 num_bytes, u32 alignment);
  // Marks final_num_bytes (at most what was reserved) as written.
  void CommitMemory(u32 final_num_bytes);

  static std::unique_ptr<StreamBuffer> Create(VkBufferUsageFlags usage, u32 size);

private:
  // A position the GPU will have read up to once `counter` completes.
  struct Mark
  {
    u64 counter;
    u32 position;
  };

  bool CreateBacking();
  void RecordWritePosition();
  void RetireCompletedMarks();
  // Whether `needed` bytes fit right now; *wrap is set when they fit only at the start.
  bool FitsWithoutWaiting(u32 needed, bool* wrap) const;
  // Waits for the oldest submitted mark that frees enough room. False when none does.
  bool WaitForClearSpace(u32 needed);

  VkBufferUsageFlags m_usage;
  u32 m_size;
  u32 m_write_pos = 0;
  u32 m_gpu_pos = 0;
  u32 m_reserved = 0;

  VkBuffer m_buffer = VK_NULL_HANDLE;
  VmaAllocation m_alloc = VK_NULL_HANDLE;
  u8* m_mapped = nullptr;

  std::deque<Mark> m_marks;
};

}  // namespace Vulkan
