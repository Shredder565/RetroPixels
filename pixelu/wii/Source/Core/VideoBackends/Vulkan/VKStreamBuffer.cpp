// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKStreamBuffer.h"

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

namespace Vulkan
{
StreamBuffer::StreamBuffer(VkBufferUsageFlags usage, u32 size) : m_usage(usage), m_size(size)
{
}

StreamBuffer::~StreamBuffer()
{
  // The mapping goes away with the allocation.
  if (m_buffer != VK_NULL_HANDLE)
    g_command_buffer_mgr->DeferBufferDestruction(m_buffer, m_alloc);
}

std::unique_ptr<StreamBuffer> StreamBuffer::Create(VkBufferUsageFlags usage, u32 size)
{
  auto stream = std::make_unique<StreamBuffer>(usage, size);
  return stream->CreateBacking() ? std::move(stream) : nullptr;
}

bool StreamBuffer::CreateBacking()
{
  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = m_size;
  buffer_info.usage = m_usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  // Written once, front to back, by the CPU. System memory is preferred: host-visible VRAM
  // tends to be slower for this pattern.
  VmaAllocationCreateInfo request = {};
  request.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  request.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;

  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  VmaAllocationInfo placed = {};
  const VkResult res = vmaCreateBuffer(g_vulkan_context->GetMemoryAllocator(), &buffer_info,
                                       &request, &buffer, &alloc, &placed);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateBuffer failed: ");
    return false;
  }

  if (m_buffer != VK_NULL_HANDLE)
    g_command_buffer_mgr->DeferBufferDestruction(m_buffer, m_alloc);

  m_buffer = buffer;
  m_alloc = alloc;
  m_mapped = static_cast<u8*>(placed.pMappedData);
  m_write_pos = 0;
  m_gpu_pos = 0;
  m_marks.clear();
  return true;
}

bool StreamBuffer::FitsWithoutWaiting(u32 needed, bool* wrap) const
{
  if (m_write_pos < m_gpu_pos)
  {
    // Writing behind the GPU: only the gap up to it is free. The gap must stay non-empty, or
    // the write position would equal the GPU position and look like "GPU caught up".
    return needed < m_gpu_pos - m_write_pos;
  }

  // Writing ahead of (or level with) the GPU: the tail of the buffer is free, and so is the
  // start up to the GPU position (strictly less, for the same reason as above).
  if (needed <= m_size - m_write_pos)
    return true;
  if (needed < m_gpu_pos)
  {
    *wrap = true;
    return true;
  }
  return false;
}

bool StreamBuffer::ReserveMemory(u32 num_bytes, u32 alignment)
{
  // Room for the data plus any alignment padding.
  const u32 needed = num_bytes + alignment;
  if (needed > m_size)
  {
    PanicAlertFmt("Attempting to allocate {} bytes from a {} byte stream buffer", num_bytes,
                  m_size);
    return false;
  }

  RecordWritePosition();

  bool wrap = false;
  if (!FitsWithoutWaiting(needed, &wrap) && !WaitForClearSpace(needed))
    return false;

  m_write_pos = wrap ? 0 : Common::AlignUp(m_write_pos, alignment);
  m_reserved = num_bytes;
  return true;
}

void StreamBuffer::CommitMemory(u32 final_num_bytes)
{
  ASSERT(m_write_pos + final_num_bytes <= m_size);
  ASSERT(final_num_bytes <= m_reserved);

  // A no-op for coherent memory.
  vmaFlushAllocation(g_vulkan_context->GetMemoryAllocator(), m_alloc, m_write_pos,
                     final_num_bytes);
  m_write_pos += final_num_bytes;
}

void StreamBuffer::RecordWritePosition()
{
  // Nothing outstanding: no mark needed.
  if (m_write_pos == m_gpu_pos)
    return;

  const u64 counter = g_command_buffer_mgr->GetCurrentFenceCounter();
  if (!m_marks.empty() && m_marks.back().counter == counter)
  {
    // Same submission as the last mark; it now ends further on.
    m_marks.back().position = m_write_pos;
    return;
  }

  // First write into a new submission: retire what the GPU has finished, then start a mark.
  RetireCompletedMarks();
  m_marks.push_back({counter, m_write_pos});
}

void StreamBuffer::RetireCompletedMarks()
{
  const u64 completed = g_command_buffer_mgr->GetCompletedFenceCounter();
  while (!m_marks.empty() && m_marks.front().counter <= completed)
  {
    m_gpu_pos = m_marks.front().position;
    m_marks.pop_front();
  }
}

bool StreamBuffer::WaitForClearSpace(u32 needed)
{
  // Look for the oldest mark that, once the GPU reaches it, leaves enough room.
  size_t chosen = m_marks.size();
  u32 next_write = 0;
  u32 next_gpu = 0;
  for (size_t index = 0; index < m_marks.size(); ++index)
  {
    const u32 mark = m_marks[index].position;
    if (mark == m_write_pos)
    {
      // The GPU will have read everything written: the whole buffer is free.
      next_write = 0;
      next_gpu = 0;
      chosen = index;
      break;
    }
    if (mark < m_write_pos)
    {
      // The GPU would be behind us: use the tail, or wrap to the start in front of it (a
      // strictly larger gap, so that we never land exactly on the GPU position).
      if (m_size - m_write_pos >= needed)
      {
        next_write = m_write_pos;
        next_gpu = mark;
        chosen = index;
        break;
      }
      if (mark > needed)
      {
        next_write = 0;
        next_gpu = mark;
        chosen = index;
        break;
      }
      continue;
    }
    // The GPU would still be ahead of us: the gap up to it must be strictly larger.
    if (mark - m_write_pos > needed)
    {
      next_write = m_write_pos;
      next_gpu = mark;
      chosen = index;
      break;
    }
  }

  // Nothing helps, or the helpful mark belongs to the unsubmitted command buffer.
  if (chosen == m_marks.size() ||
      m_marks[chosen].counter == g_command_buffer_mgr->GetCurrentFenceCounter())
  {
    return false;
  }

  g_command_buffer_mgr->WaitForFenceCounter(m_marks[chosen].counter);

  // When the GPU catches up completely every mark is done; otherwise drop the ones up to and
  // including the one waited for.
  if (m_marks[chosen].position == m_write_pos)
    m_marks.clear();
  else
    m_marks.erase(m_marks.begin(), m_marks.begin() + static_cast<std::ptrdiff_t>(chosen + 1));

  m_write_pos = next_write;
  m_gpu_pos = next_gpu;
  return true;
}

}  // namespace Vulkan
