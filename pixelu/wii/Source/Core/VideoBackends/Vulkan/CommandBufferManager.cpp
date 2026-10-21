// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/CommandBufferManager.h"

#include <array>
#include <cstdint>
#include <type_traits>

#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/Constants.h"

namespace Vulkan
{
std::unique_ptr<CommandBufferManager> g_command_buffer_mgr;

namespace
{
VkDevice Device()
{
  return g_vulkan_context->GetDevice();
}

// Logs a failed call made where there is nothing better to do than carry on.
void Check(VkResult res, const char* what)
{
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, what);
}

// Ending or submitting a command buffer must not fail; the user is told when it does.
void CheckFatal(VkResult res, const char* what, const char* alert)
{
  if (res == VK_SUCCESS)
    return;
  LOG_VULKAN_ERROR(res, what);
  PanicAlertFmt("{}: {} ({})", alert, VkResultToString(res), static_cast<int>(res));
}
}  // namespace

CommandBufferManager::CommandBufferManager(bool use_threaded_submission)
    : m_threaded(use_threaded_submission)
{
}

CommandBufferManager::~CommandBufferManager()
{
  if (m_threaded)
  {
    WaitForWorkerThreadIdle();
    m_worker.Shutdown();
  }
  DestroySlots();
}

bool CommandBufferManager::Initialize(size_t swapchain_image_count)
{
  if (!CreateSlots(swapchain_image_count))
    return false;
  if (m_threaded)
    StartWorker();
  return true;
}

bool CommandBufferManager::CreateSlots(size_t swapchain_image_count)
{
  const VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, nullptr,
                                                0};

  for (Slot& slot : m_slots)
  {
    slot.init_buffer_used = false;
    slot.wait_on_acquire = false;

    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = g_vulkan_context->GetGraphicsQueueFamilyIndex();
    VkResult res = vkCreateCommandPool(Device(), &pool_info, nullptr, &slot.pool);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateCommandPool failed: ");
      return false;
    }

    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = slot.pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<u32>(slot.buffers.size());
    res = vkAllocateCommandBuffers(Device(), &alloc_info, slot.buffers.data());
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkAllocateCommandBuffers failed: ");
      return false;
    }

    // Created signalled: the first wait on a never-used slot must not block.
    const VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                                          VK_FENCE_CREATE_SIGNALED_BIT};
    res = vkCreateFence(Device(), &fence_info, nullptr, &slot.fence);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateFence failed: ");
      return false;
    }

    res = vkCreateSemaphore(Device(), &semaphore_info, nullptr, &slot.acquire_semaphore);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateSemaphore failed: ");
      return false;
    }
  }

  m_render_done.reserve(swapchain_image_count);
  for (size_t image = 0; image < swapchain_image_count; image++)
  {
    VkSemaphore semaphore;
    const VkResult res = vkCreateSemaphore(Device(), &semaphore_info, nullptr, &semaphore);
    if (res != VK_SUCCESS)
    {
      LOG_VULKAN_ERROR(res, "vkCreateSemaphore failed: ");
      return false;
    }
    m_render_done.push_back(semaphore);
  }

  // BeginNextSlot() advances first, so park on the last slot to start recording into slot 0.
  m_recording = static_cast<u32>(NUM_COMMAND_BUFFERS) - 1;
  BeginNextSlot();
  return true;
}

void CommandBufferManager::DestroySlots()
{
  for (Slot& slot : m_slots)
  {
    // Destroying the pool frees its command buffers. It goes first so nothing still refers to
    // the objects released next.
    if (slot.pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(Device(), slot.pool, nullptr);
    for (const DeferredObject& object : slot.deferred)
      DestroyDeferred(object);
    if (slot.acquire_semaphore != VK_NULL_HANDLE)
      vkDestroySemaphore(Device(), slot.acquire_semaphore, nullptr);
    if (slot.fence != VK_NULL_HANDLE)
      vkDestroyFence(Device(), slot.fence, nullptr);
  }

  for (const FramePools& frame : m_frames)
  {
    for (VkDescriptorPool pool : frame.pools)
      vkDestroyDescriptorPool(Device(), pool, nullptr);
  }

  for (VkSemaphore semaphore : m_render_done)
    vkDestroySemaphore(Device(), semaphore, nullptr);
}

VkDescriptorPool CommandBufferManager::CreateDescriptorPool(u32 max_sets)
{
  // Enough of every type for max_sets of the largest layouts in ObjectCache: three dynamic
  // UBOs, every pixel, compute and utility sampler, two SSBOs, three texel buffers and an image.
  constexpr u32 samplers_per_set = VideoCommon::MAX_PIXEL_SHADER_SAMPLERS +
                                   VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS +
                                   NUM_UTILITY_PIXEL_SAMPLERS;
  const std::array<VkDescriptorPoolSize, 5> sizes = {{
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, max_sets * 3},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_sets * samplers_per_set},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, max_sets * 2},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, max_sets * 3},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, max_sets},
  }};

  VkDescriptorPoolCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  info.maxSets = max_sets;
  info.poolSizeCount = static_cast<u32>(sizes.size());
  info.pPoolSizes = sizes.data();

  VkDescriptorPool pool = VK_NULL_HANDLE;
  const VkResult res = vkCreateDescriptorPool(Device(), &info, nullptr, &pool);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateDescriptorPool failed: ");
    return VK_NULL_HANDLE;
  }
  return pool;
}

VkDescriptorSet CommandBufferManager::AllocateDescriptorSet(VkDescriptorSetLayout set_layout)
{
  FramePools& frame = m_frames[m_frame];
  VkDescriptorSet set = VK_NULL_HANDLE;

  if (!frame.pools.empty()) [[likely]]
  {
    VkDescriptorSetAllocateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = frame.pools[frame.active];
    info.descriptorSetCount = 1;
    info.pSetLayouts = &set_layout;
    const VkResult res = vkAllocateDescriptorSets(Device(), &info, &set);

    // A full pool moves on to the next one this frame already has.
    if (res != VK_SUCCESS && frame.active + 1 < frame.pools.size())
    {
      frame.active++;
      set = AllocateDescriptorSet(set_layout);
    }
  }

  if (set != VK_NULL_HANDLE) [[likely]]
    return set;

  // Out of pools: add one more. The frame's pools are merged into one pool of the combined
  // size the next time this frame comes round.
  const VkDescriptorPool pool = CreateDescriptorPool(DESCRIPTOR_SETS_PER_POOL);
  m_pool_capacity += DESCRIPTOR_SETS_PER_POOL;
  if (pool == VK_NULL_HANDLE) [[unlikely]]
    return VK_NULL_HANDLE;

  frame.pools.push_back(pool);
  frame.active = static_cast<u32>(frame.pools.size()) - 1;
  return AllocateDescriptorSet(set_layout);
}

void CommandBufferManager::StartWorker()
{
  m_worker.Reset("VK submission thread", [this](QueuedSubmit work) {
    QueueSlot(work.slot, work.swap_chain, work.image_index);
    m_slots[work.slot].queued.store(false, std::memory_order_release);
  });
}

void CommandBufferManager::WaitForWorkerThreadIdle()
{
  if (m_threaded)
    m_worker.WaitForCompletion();
}

void CommandBufferManager::WaitForFenceCounter(u64 fence_counter)
{
  if (fence_counter <= m_completed_counter)
    return;

  // The oldest submitted slot whose counter covers the request.
  u32 slot = NextSlot(m_recording);
  for (; slot != m_recording; slot = NextSlot(slot))
  {
    if (m_slots[slot].fence_counter >= fence_counter)
      break;
  }
  ASSERT(slot != m_recording);
  WaitForSlot(slot);
}

void CommandBufferManager::WaitForSlot(u32 index)
{
  Slot& slot = m_slots[index];

  // It must have reached the queue before its fence means anything.
  if (slot.queued.load(std::memory_order_acquire))
  {
    WaitForWorkerThreadIdle();
    ASSERT_MSG(VIDEO, !slot.queued.load(std::memory_order_relaxed),
               "Submit thread is idle but command buffer is still waiting for submission!");
  }

  Check(vkWaitForFences(Device(), 1, &slot.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences failed: ");

  // Everything submitted up to this slot has finished now; release what those slots held,
  // oldest first.
  const u64 finished = slot.fence_counter;
  for (u32 older = NextSlot(m_recording); older != m_recording; older = NextSlot(older))
  {
    Slot& done = m_slots[older];
    if (done.fence_counter > finished)
      break;
    if (done.fence_counter <= m_completed_counter)
      continue;

    for (const DeferredObject& object : done.deferred)
      DestroyDeferred(object);
    done.deferred.clear();
  }

  m_completed_counter = finished;
}

void CommandBufferManager::SubmitCommandBuffer(bool submit_on_worker_thread,
                                               bool wait_for_completion, bool advance_to_next_frame,
                                               VkSwapchainKHR present_swap_chain,
                                               uint32_t present_image_index)
{
  Slot& slot = m_slots[m_recording];
  for (VkCommandBuffer buffer : slot.buffers)
  {
    CheckFatal(vkEndCommandBuffer(buffer), "vkEndCommandBuffer failed: ",
               "Failed to end command buffer");
  }

  const bool hand_off = m_threaded && submit_on_worker_thread && !wait_for_completion;
  if (hand_off)
  {
    slot.queued.store(true, std::memory_order_relaxed);
    m_worker.Push({present_swap_chain, present_image_index, m_recording});
  }
  else
  {
    // Keep queue order: anything still with the worker goes first.
    WaitForWorkerThreadIdle();
    QueueSlot(m_recording, present_swap_chain, present_image_index);
    if (wait_for_completion)
      WaitForSlot(m_recording);
  }

  if (advance_to_next_frame)
    RecycleFramePools();

  BeginNextSlot();
}

void CommandBufferManager::RecycleFramePools()
{
  m_frame = (m_frame + 1) % static_cast<u32>(NUM_FRAMES_IN_FLIGHT);

  // The pools about to be reused may still be referenced by unfinished slots of that frame.
  for (u32 index = NextSlot(m_recording); index != m_recording; index = NextSlot(index))
  {
    const Slot& slot = m_slots[index];
    const bool pending = slot.fence_counter != 0 && slot.fence_counter > m_completed_counter;
    if (slot.frame == m_frame && pending)
      WaitForSlot(index);
  }

  FramePools& frame = m_frames[m_frame];
  if (frame.pools.size() == 1) [[likely]]
  {
    Check(vkResetDescriptorPool(Device(), frame.pools.front(), 0),
          "vkResetDescriptorPool failed: ");
  }
  else [[unlikely]]
  {
    // Replace several pools (or none) by a single pool sized for all sets handed out so far.
    for (VkDescriptorPool pool : frame.pools)
      vkDestroyDescriptorPool(Device(), pool, nullptr);
    frame.pools.clear();
    const VkDescriptorPool merged = CreateDescriptorPool(m_pool_capacity);
    if (merged != VK_NULL_HANDLE) [[likely]]
      frame.pools.push_back(merged);
  }
  frame.active = 0;
}

void CommandBufferManager::QueueSlot(u32 index, VkSwapchainKHR swap_chain, u32 image_index)
{
  // May run on the worker thread: reads the slot, writes only the present result.
  const Slot& slot = m_slots[index];

  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit = {};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.pWaitDstStageMask = &wait_stage;
  if (slot.init_buffer_used)
  {
    submit.commandBufferCount = static_cast<u32>(slot.buffers.size());
    submit.pCommandBuffers = slot.buffers.data();
  }
  else
  {
    // Nothing was recorded into the init buffer; submit the draw buffer alone.
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.buffers[DRAW_BUFFER];
  }
  if (slot.wait_on_acquire)
  {
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &slot.acquire_semaphore;
  }
  if (swap_chain != VK_NULL_HANDLE)
  {
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &m_render_done[image_index];
  }

  CheckFatal(vkQueueSubmit(g_vulkan_context->GetGraphicsQueue(), 1, &submit, slot.fence),
             "vkQueueSubmit failed: ", "Failed to submit command buffer");

  if (swap_chain == VK_NULL_HANDLE)
    return;

  VkPresentInfoKHR present = {};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &m_render_done[image_index];
  present.swapchainCount = 1;
  present.pSwapchains = &swap_chain;
  present.pImageIndices = &image_index;

  m_last_present_result = vkQueuePresentKHR(g_vulkan_context->GetPresentQueue(), &present);
  if (m_last_present_result == VK_SUCCESS)
    return;

  // Out-of-date, suboptimal and lost exclusive fullscreen are expected; the swap chain is
  // rebuilt on the next frame either way.
  const bool expected = m_last_present_result == VK_ERROR_OUT_OF_DATE_KHR ||
                        m_last_present_result == VK_SUBOPTIMAL_KHR ||
                        m_last_present_result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;
  if (!expected)
    LOG_VULKAN_ERROR(m_last_present_result, "vkQueuePresentKHR failed: ");
  m_present_failed.Set();
}

void CommandBufferManager::BeginNextSlot()
{
  const u32 next = NextSlot(m_recording);
  Slot& slot = m_slots[next];

  // The slot's previous work has to be finished before its pool and fence are reset.
  if (slot.fence_counter > m_completed_counter)
    WaitForSlot(next);

  Check(vkResetFences(Device(), 1, &slot.fence), "vkResetFences failed: ");
  Check(vkResetCommandPool(Device(), slot.pool, 0), "vkResetCommandPool failed: ");

  VkCommandBufferBeginInfo begin = {};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  for (VkCommandBuffer buffer : slot.buffers)
    Check(vkBeginCommandBuffer(buffer, &begin), "vkBeginCommandBuffer failed: ");

  slot.init_buffer_used = false;
  slot.wait_on_acquire = false;
  slot.fence_counter = m_next_counter++;
  slot.frame = m_frame;
  m_recording = next;
}

void CommandBufferManager::Defer(DeferredObject object)
{
  m_slots[m_recording].deferred.push_back(object);
}

void CommandBufferManager::DestroyDeferred(const DeferredObject& object)
{
  std::visit(
      [](const auto& dead) {
        using T = std::decay_t<decltype(dead)>;
        if constexpr (std::is_same_v<T, DeadBufferView>)
          vkDestroyBufferView(Device(), dead.view, nullptr);
        else if constexpr (std::is_same_v<T, DeadBuffer>)
          vmaDestroyBuffer(g_vulkan_context->GetMemoryAllocator(), dead.buffer, dead.alloc);
        else if constexpr (std::is_same_v<T, DeadFramebuffer>)
          vkDestroyFramebuffer(Device(), dead.framebuffer, nullptr);
        else if constexpr (std::is_same_v<T, DeadImage>)
          vmaDestroyImage(g_vulkan_context->GetMemoryAllocator(), dead.image, dead.alloc);
        else
          vkDestroyImageView(Device(), dead.view, nullptr);
      },
      object);
}

void CommandBufferManager::DeferBufferViewDestruction(VkBufferView object)
{
  Defer(DeadBufferView{object});
}

void CommandBufferManager::DeferBufferDestruction(VkBuffer buffer, VmaAllocation alloc)
{
  Defer(DeadBuffer{buffer, alloc});
}

void CommandBufferManager::DeferFramebufferDestruction(VkFramebuffer object)
{
  Defer(DeadFramebuffer{object});
}

void CommandBufferManager::DeferImageDestruction(VkImage object, VmaAllocation alloc)
{
  Defer(DeadImage{object, alloc});
}

void CommandBufferManager::DeferImageViewDestruction(VkImageView object)
{
  Defer(DeadImageView{object});
}

}  // namespace Vulkan
