// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <variant>
#include <vector>

#include "Common/Flag.h"
#include "Common/WorkQueueThread.h"

#include "VideoBackends/Vulkan/Constants.h"

namespace Vulkan
{
// Records GPU work into a ring of NUM_COMMAND_BUFFERS slots. Each slot has an "init" command
// buffer (uploads, executed first) and a draw command buffer, a fence and an acquire
// semaphore. Every submission gets a fence counter; a counter at or below
// GetCompletedFenceCounter() means that work has finished on the GPU. Submissions can be
// handed to a worker thread, which then also presents.
class CommandBufferManager
{
public:
  explicit CommandBufferManager(bool use_threaded_submission);
  ~CommandBufferManager();

  bool Initialize(size_t swapchain_image_count);

  // Command buffers of the slot being recorded; valid until the next submission.
  VkCommandBuffer GetCurrentInitCommandBuffer()
  {
    Slot& slot = m_slots[m_recording];
    slot.init_buffer_used = true;
    return slot.buffers[INIT_BUFFER];
  }
  VkCommandBuffer GetCurrentCommandBuffer() const { return m_slots[m_recording].buffers[DRAW_BUFFER]; }

  // A descriptor set from the current frame's pools (they are reset two frames later).
  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout set_layout);

  u64 GetCompletedFenceCounter() const { return m_completed_counter; }
  // The counter the work being recorded now will complete with.
  u64 GetCurrentFenceCounter() const { return m_slots[m_recording].fence_counter; }

  // The semaphore a swap chain acquire signals for the slot being recorded. Call
  // MarkCurrentCommandBufferSemaphoreUsed() once it will be signalled, so that the submission
  // waits on it.
  VkSemaphore GetCurrentCommandBufferSemaphore() { return m_slots[m_recording].acquire_semaphore; }
  void MarkCurrentCommandBufferSemaphoreUsed() { m_slots[m_recording].wait_on_acquire = true; }

  // Returns once the worker thread has submitted everything queued to it.
  void WaitForWorkerThreadIdle();

  // Blocks until the work with this counter has finished, running deferred destructions.
  void WaitForFenceCounter(u64 fence_counter);

  // Ends the slot being recorded, submits it (on the worker thread when asked and allowed),
  // optionally presents, optionally moves to the next frame's descriptor pools, and starts
  // recording the next slot.
  void SubmitCommandBuffer(bool submit_on_worker_thread, bool wait_for_completion,
                           bool advance_to_next_frame = false,
                           VkSwapchainKHR present_swap_chain = VK_NULL_HANDLE,
                           uint32_t present_image_index = 0xFFFFFFFF);

  // True (once) after a present that failed; the swap chain then needs recreating.
  bool CheckLastPresentFail() { return m_present_failed.TestAndClear(); }
  VkResult GetLastPresentResult() const { return m_last_present_result; }

  // Destroys the object once the GPU is done with the slot being recorded now.
  void DeferBufferViewDestruction(VkBufferView object);
  void DeferBufferDestruction(VkBuffer buffer, VmaAllocation alloc);
  void DeferFramebufferDestruction(VkFramebuffer object);
  void DeferImageDestruction(VkImage object, VmaAllocation alloc);
  void DeferImageViewDestruction(VkImageView object);

private:
  static constexpr size_t INIT_BUFFER = 0;
  static constexpr size_t DRAW_BUFFER = 1;
  static constexpr u32 DESCRIPTOR_SETS_PER_POOL = 1024;

  // Objects waiting for the GPU before they can be destroyed.
  struct DeadBufferView
  {
    VkBufferView view;
  };
  struct DeadBuffer
  {
    VkBuffer buffer;
    VmaAllocation alloc;
  };
  struct DeadFramebuffer
  {
    VkFramebuffer framebuffer;
  };
  struct DeadImage
  {
    VkImage image;
    VmaAllocation alloc;
  };
  struct DeadImageView
  {
    VkImageView view;
  };
  using DeferredObject =
      std::variant<DeadBufferView, DeadBuffer, DeadFramebuffer, DeadImage, DeadImageView>;

  struct Slot
  {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 2> buffers = {};
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquire_semaphore = VK_NULL_HANDLE;
    u64 fence_counter = 0;
    u32 frame = 0;
    bool init_buffer_used = false;
    bool wait_on_acquire = false;
    // Set while the slot sits in the worker thread's queue.
    std::atomic<bool> queued{false};
    std::vector<DeferredObject> deferred;
  };

  struct FramePools
  {
    std::vector<VkDescriptorPool> pools;
    u32 active = 0;
  };

  struct QueuedSubmit
  {
    VkSwapchainKHR swap_chain;
    u32 image_index;
    u32 slot;
  };

  bool CreateSlots(size_t swapchain_image_count);
  void DestroySlots();
  void StartWorker();

  u32 NextSlot(u32 slot) const { return (slot + 1) % static_cast<u32>(NUM_COMMAND_BUFFERS); }
  void WaitForSlot(u32 slot);
  void QueueSlot(u32 slot, VkSwapchainKHR swap_chain, u32 image_index);
  void BeginNextSlot();
  void RecycleFramePools();
  void Defer(DeferredObject object);
  static void DestroyDeferred(const DeferredObject& object);

  VkDescriptorPool CreateDescriptorPool(u32 max_sets);

  std::array<Slot, NUM_COMMAND_BUFFERS> m_slots;
  std::array<FramePools, NUM_FRAMES_IN_FLIGHT> m_frames;
  u32 m_recording = 0;
  u32 m_frame = 0;
  u64 m_next_counter = 1;
  u64 m_completed_counter = 0;
  u32 m_pool_capacity = DESCRIPTOR_SETS_PER_POOL;

  // One "rendering finished" semaphore per swap chain image.
  std::vector<VkSemaphore> m_render_done;

  bool m_threaded = false;
  Common::WorkQueueThreadSP<QueuedSubmit> m_worker;
  Common::Flag m_present_failed;
  VkResult m_last_present_result = VK_SUCCESS;
};

extern std::unique_ptr<CommandBufferManager> g_command_buffer_mgr;

}  // namespace Vulkan
