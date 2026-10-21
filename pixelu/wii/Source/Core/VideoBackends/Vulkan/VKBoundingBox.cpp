// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKBoundingBox.h"

#include <vector>

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/ObjectCache.h"
#include "VideoBackends/Vulkan/StagingBuffer.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VulkanContext.h"

namespace Vulkan
{
namespace
{
constexpr VkAccessFlags SHADER_RW = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

VkCommandBuffer Cmd()
{
  return g_command_buffer_mgr->GetCurrentCommandBuffer();
}
}  // namespace

VKBoundingBox::~VKBoundingBox()
{
  if (m_buffer != VK_NULL_HANDLE)
    vmaDestroyBuffer(g_vulkan_context->GetMemoryAllocator(), m_buffer, m_allocation);
}

bool VKBoundingBox::Initialize()
{
  VkBufferCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = BUFFER_SIZE;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VmaAllocationCreateInfo request = {};
  request.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
  request.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  const VkResult res = vmaCreateBuffer(g_vulkan_context->GetMemoryAllocator(), &info, &request,
                                       &buffer, &allocation, nullptr);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vmaCreateBuffer failed: ");
    return false;
  }
  m_buffer = buffer;
  m_allocation = allocation;

  m_readback = StagingBuffer::Create(STAGING_BUFFER_TYPE_READBACK, BUFFER_SIZE,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  if (m_readback == nullptr || !m_readback->Map())
    return false;

  StateTracker::GetInstance()->SetSSBO(m_buffer, 0, BUFFER_SIZE);
  return true;
}

void VKBoundingBox::ShaderToTransfer(VkAccessFlags transfer_access)
{
  StagingBuffer::BufferMemoryBarrier(Cmd(), m_buffer, SHADER_RW, transfer_access, 0, BUFFER_SIZE,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void VKBoundingBox::TransferToShader(VkAccessFlags transfer_access)
{
  StagingBuffer::BufferMemoryBarrier(Cmd(), m_buffer, transfer_access, SHADER_RW, 0, BUFFER_SIZE,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

std::vector<BBoxType> VKBoundingBox::Read(u32 index, u32 length)
{
  // Transfers are not allowed inside a render pass.
  StateTracker::GetInstance()->EndRenderPass();

  ShaderToTransfer(VK_ACCESS_TRANSFER_READ_BIT);
  m_readback->PrepareForGPUWrite(Cmd(), VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT);
  const VkBufferCopy whole = {0, 0, BUFFER_SIZE};
  vkCmdCopyBuffer(Cmd(), m_buffer, m_readback->GetBuffer(), 1, &whole);
  TransferToShader(VK_ACCESS_TRANSFER_READ_BIT);
  m_readback->FlushGPUCache(Cmd(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

  // Submit and wait, then pull the GPU's writes into the CPU's view.
  VKGfx::GetInstance()->ExecuteCommandBuffer(false, true);
  m_readback->InvalidateCPUCache();

  std::vector<BBoxType> values(length);
  m_readback->Read(index * sizeof(BBoxType), values.data(), length * sizeof(BBoxType), false);
  return values;
}

void VKBoundingBox::Write(u32 index, std::span<const BBoxType> values)
{
  // Updates must stay ordered with the draws, so they go into the draw command buffer (outside
  // any render pass), not the init buffer.
  StateTracker::GetInstance()->EndRenderPass();

  ShaderToTransfer(VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdUpdateBuffer(Cmd(), m_buffer, index * sizeof(BBoxType), values.size() * sizeof(BBoxType),
                    values.data());
  TransferToShader(VK_ACCESS_TRANSFER_WRITE_BIT);
}

}  // namespace Vulkan
