// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <memory>

#include "VideoBackends/Vulkan/Constants.h"

namespace Vulkan
{
// A host-visible buffer for moving data between the CPU and the GPU. It is mapped for its whole
// life (VMA keeps the mapping); the cache and barrier helpers below cover the two directions:
//   upload:   Write (flushes the CPU cache), then optionally InvalidateGPUCache;
//   readback: PrepareForGPUWrite, the GPU copy, FlushGPUCache, then Read (invalidates).
class StagingBuffer final
{
public:
  StagingBuffer(STAGING_BUFFER_TYPE type, VkBuffer buffer, VmaAllocation allocation,
                VkDeviceSize size, char* map_ptr);
  ~StagingBuffer();

  STAGING_BUFFER_TYPE GetType() const { return m_type; }
  VkDeviceSize GetSize() const { return m_size; }
  VkBuffer GetBuffer() const { return m_buffer; }
  bool IsMapped() const { return m_map_pointer != nullptr; }
  const char* GetMapPointer() const { return m_map_pointer; }
  char* GetMapPointer() { return m_map_pointer; }
  // The buffer is always mapped; these exist for callers written against map/unmap.
  bool Map();
  void Unmap();

  void FlushCPUCache(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);
  // Makes host writes visible to a GPU stage. Submitting does this implicitly.
  void InvalidateGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits dst_access_flags,
                          VkPipelineStageFlagBits dst_pipeline_stage, VkDeviceSize offset = 0,
                          VkDeviceSize size = VK_WHOLE_SIZE);
  // Orders earlier accesses before a GPU write into the buffer.
  void PrepareForGPUWrite(VkCommandBuffer command_buffer, VkAccessFlagBits dst_access_flags,
                          VkPipelineStageFlagBits dst_pipeline_stage, VkDeviceSize offset = 0,
                          VkDeviceSize size = VK_WHOLE_SIZE);
  // Makes a GPU write visible to host reads.
  void FlushGPUCache(VkCommandBuffer command_buffer, VkAccessFlagBits src_access_flags,
                     VkPipelineStageFlagBits src_pipeline_stage, VkDeviceSize offset = 0,
                     VkDeviceSize size = VK_WHOLE_SIZE);
  void InvalidateCPUCache(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);

  // Offsets are from the start of the buffer.
  void Read(VkDeviceSize offset, void* data, size_t size, bool invalidate_caches = true);
  void Write(VkDeviceSize offset, const void* data, size_t size, bool invalidate_caches = true);

  static std::unique_ptr<StagingBuffer> Create(STAGING_BUFFER_TYPE type, VkDeviceSize size,
                                               VkBufferUsageFlags usage);

  // Creates the buffer and its mapped allocation without wrapping them in an object.
  static bool AllocateBuffer(STAGING_BUFFER_TYPE type, VkDeviceSize size, VkBufferUsageFlags usage,
                             VkBuffer* out_buffer, VmaAllocation* out_alloc, char** out_map_ptr);

  // Records a single buffer memory barrier (no queue family transfer).
  static void BufferMemoryBarrier(VkCommandBuffer command_buffer, VkBuffer buffer,
                                  VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
                                  VkDeviceSize offset, VkDeviceSize size,
                                  VkPipelineStageFlags src_stage_mask,
                                  VkPipelineStageFlags dst_stage_mask);

private:
  bool RangeFits(VkDeviceSize offset, VkDeviceSize size) const;

  STAGING_BUFFER_TYPE m_type;
  VkBuffer m_buffer;
  VmaAllocation m_alloc;
  VkDeviceSize m_size;
  char* m_map_pointer = nullptr;
};
}  // namespace Vulkan
