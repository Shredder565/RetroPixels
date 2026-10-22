// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/StagingBuffer.h"

#include <cstring>

#include "Common/Assert.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/DriverDetails.h"

namespace Vulkan
{
StagingBuffer::StagingBuffer(STAGING_BUFFER_TYPE type, VkBuffer buffer, VmaAllocation allocation,
                             VkDeviceSize size, char* map_ptr)
    : m_type(type), m_buffer(buffer), m_alloc(allocation), m_size(size), m_map_pointer(map_ptr)
{
}

StagingBuffer::~StagingBuffer()
{
  // The GPU may still be reading or writing it.
  g_command_buffer_mgr->DeferBufferDestruction(m_buffer, m_alloc);
}

bool StagingBuffer::RangeFits(VkDeviceSize offset, VkDeviceSize size) const
{
  if (size == VK_WHOLE_SIZE)
    return offset < m_size;
  return offset + size <= m_size;
}

void StagingBuffer::BufferMemoryBarrier(VkCommandBuffer command_buffer, VkBuffer buffer,
                                        VkAccessFlags src_access_mask,
                                        VkAccessFlags dst_access_mask, VkDeviceSize offset,
                                        VkDeviceSize size, VkPipelineStageFlags src_stage_mask,
                                        VkPipelineStageFlags dst_stage_mask)
{
  VkBufferMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcAccessMask = src_access_mask;
  barrier.dstAccessMask = dst_access_mask;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = buffer;
  barrier.offset = offset;
  barrier.size = size;
  vkCmdPipelineBarrier(command_buffer, src_stage_mask, dst_stage_mask, 0, 0, nullptr, 1, &barrier,
                       0, nullptr);
}

bool StagingBuffer::Map()
{
  return true;
}

void StagingBuffer::Unmap()
{
}

void StagingBuffer::FlushCPUCache(VkDeviceSize offset, VkDeviceSize size)
{
  // VMA skips the flush for coherent memory.
  vmaFlushAllocation(g_vulkan_context->GetMemoryAllocator(), m_alloc, offset, size);
}

void StagingBuffer::InvalidateCPUCache(VkDeviceSize offset, VkDeviceSize size)
{
  // VMA skips the invalidate for coherent memory.
  vmaInvalidateAllocation(g_vulkan_context->GetMemoryAllocator(), m_alloc, offset, size);
}

void StagingBuffer::InvalidateGPUCache(VkCommandBuffer command_buffer,
                                       VkAccessFlagBits dst_access_flags,
                                       VkPipelineStageFlagBits dst_pipeline_stage,
                                       VkDeviceSize offset, VkDeviceSize size)
{
  ASSERT(RangeFits(offset, size));
  BufferMemoryBarrier(command_buffer, m_buffer, VK_ACCESS_HOST_WRITE_BIT, dst_access_flags, offset,
                      size, VK_PIPELINE_STAGE_HOST_BIT, dst_pipeline_stage);
}

void StagingBuffer::PrepareForGPUWrite(VkCommandBuffer command_buffer,
                                       VkAccessFlagBits dst_access_flags,
                                       VkPipelineStageFlagBits dst_pipeline_stage,
                                       VkDeviceSize offset, VkDeviceSize size)
{
  ASSERT(RangeFits(offset, size));
  BufferMemoryBarrier(command_buffer, m_buffer, VK_ACCESS_MEMORY_WRITE_BIT, dst_access_flags,
                      offset, size, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst_pipeline_stage);
}

void StagingBuffer::FlushGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits src_access_flags,
                                  VkPipelineStageFlagBits src_pipeline_stage, VkDeviceSize offset,
                                  VkDeviceSize size)
{
  ASSERT(RangeFits(offset, size));
  BufferMemoryBarrier(command_buffer, m_buffer, src_access_flags, VK_ACCESS_HOST_READ_BIT, offset,
                      size, src_pipeline_stage, VK_PIPELINE_STAGE_HOST_BIT);
}

void StagingBuffer::Read(VkDeviceSize offset, void* data, size_t size, bool invalidate_caches)
{
  ASSERT(offset + size <= m_size);
  if (invalidate_caches)
    InvalidateCPUCache(offset, size);
  std::memcpy(data, m_map_pointer + offset, size);
}

void StagingBuffer::Write(VkDeviceSize offset, const void* data, size_t size,
                          bool invalidate_caches)
{
  ASSERT(offset + size <= m_size);
  std::memcpy(m_map_pointer + offset, data, size);
  if (invalidate_caches)
    FlushCPUCache(offset, size);
}

bool StagingBuffer::AllocateBuffer(STAGING_BUFFER_TYPE type, VkDeviceSize size,
                                   VkBufferUsageFlags usage, VkBuffer* out_buffer,
                                   VmaAllocation* out_alloc, char** out_map_ptr)
{
  VkBufferCreateInfo buffer_info = {};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = usage;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo request = {};
  request.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  if (DriverDetails::HasBug(DriverDetails::BUG_SLOW_CACHED_READBACK_MEMORY)) [[unlikely]]
  {
    // Cached and coherent together is preferred, coherent alone is acceptable.
    request.usage = VMA_MEMORY_USAGE_UNKNOWN;
    request.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    request.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  }
  else
  {
    request.usage = VMA_MEMORY_USAGE_AUTO;
    request.flags |= type == STAGING_BUFFER_TYPE_UPLOAD ?
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT :
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
  }

  const VmaAllocator allocator = g_vulkan_context->GetMemoryAllocator();
  VmaAllocationInfo placed = {};
  const VkResult res =
      vmaCreateBuffer(allocator, &buffer_info, &request, out_buffer, out_alloc, &placed);

  // Uploads into non-coherent memory need explicit flushes, which cost time; say so once per
  // buffer (this check runs before the result is looked at, as it always has).
  if (type == STAGING_BUFFER_TYPE_UPLOAD)
  {
    VkMemoryPropertyFlags type_flags = 0;
    vmaGetMemoryTypeProperties(allocator, placed.memoryType, &type_flags);
    if ((type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
    {
      WARN_LOG_FMT(VIDEO, "Vulkan: Failed to find a coherent memory type for uploads, this will "
                          "affect performance.");
    }
  }

  *out_map_ptr = static_cast<char*>(placed.pMappedData);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateBuffer failed: ");
    return false;
  }

  VkMemoryPropertyFlags alloc_flags = 0;
  vmaGetAllocationMemoryProperties(allocator, *out_alloc, &alloc_flags);
  return true;
}

std::unique_ptr<StagingBuffer> StagingBuffer::Create(STAGING_BUFFER_TYPE type, VkDeviceSize size,
                                                     VkBufferUsageFlags usage)
{
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation alloc = VK_NULL_HANDLE;
  char* map_ptr = nullptr;
  if (!AllocateBuffer(type, size, usage, &buffer, &alloc, &map_ptr))
    return nullptr;
  return std::make_unique<StagingBuffer>(type, buffer, alloc, size, map_ptr);
}

}  // namespace Vulkan
