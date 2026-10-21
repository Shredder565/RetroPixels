// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Vulkan/VKPerfQuery.h"

#include "Common/Assert.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/Vulkan/CommandBufferManager.h"
#include "VideoBackends/Vulkan/StateTracker.h"
#include "VideoBackends/Vulkan/VKGfx.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace Vulkan
{
namespace
{
bool IsOcclusionGroup(PerfQueryGroup group)
{
  return group == PQG_ZCOMP_ZCOMPLOC || group == PQG_ZCOMP;
}

VkCommandBuffer Cmd()
{
  return g_command_buffer_mgr->GetCurrentCommandBuffer();
}

// Scales a sample count at the internal resolution (and MSAA) back to native EFB pixels.
u32 ToNativePixels(u32 samples)
{
  u64 pixels = static_cast<u64>(samples) * EFB_WIDTH / g_framebuffer_manager->GetEFBWidth() *
               EFB_HEIGHT / g_framebuffer_manager->GetEFBHeight();
  if (g_ActiveConfig.iMultisamples > 1)
    pixels /= static_cast<u64>(g_ActiveConfig.iMultisamples);
  return static_cast<u32>(pixels);
}
}  // namespace

PerfQuery::PerfQuery() = default;

PerfQuery::~PerfQuery()
{
  if (m_pool != VK_NULL_HANDLE)
    vkDestroyQueryPool(g_vulkan_context->GetDevice(), m_pool, nullptr);
}

bool PerfQuery::Initialize()
{
  VkQueryPoolCreateInfo info = {};
  info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  info.queryType = VK_QUERY_TYPE_OCCLUSION;
  info.queryCount = RING_SIZE;
  const VkResult res = vkCreateQueryPool(g_vulkan_context->GetDevice(), &info, nullptr, &m_pool);
  if (res != VK_SUCCESS)
  {
    LOG_VULKAN_ERROR(res, "vkCreateQueryPool failed: ");
    PanicAlertFmt("Failed to create query pool");
    return false;
  }

  // New pools must be reset before first use.
  ResetQuery();
  return true;
}

void PerfQuery::EnableQuery(PerfQueryGroup group)
{
  // Keep about half the ring free; when it is completely full, wait.
  const u32 pending = m_query_count.load(std::memory_order_relaxed);
  if (pending > RING_SIZE / 2)
    PartialFlush(pending == RING_SIZE);

  // Record pending state first, so that no submission happens while the query is open.
  StateTracker::GetInstance()->Bind();

  if (!IsOcclusionGroup(group))
    return;

  Slot& slot = m_slots[m_back];
  DEBUG_ASSERT(!slot.in_use);
  slot.in_use = true;
  slot.group = group;

  // Without precise queries the result is only zero or non-zero (and so wrong).
  const VkQueryControlFlags flags =
      g_vulkan_context->SupportsPreciseOcclusionQueries() ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
  StateTracker::GetInstance()->BeginRenderPass();
  vkCmdBeginQuery(Cmd(), m_pool, m_back, flags);
}

void PerfQuery::DisableQuery(PerfQueryGroup group)
{
  if (!IsOcclusionGroup(group))
    return;

  vkCmdEndQuery(Cmd(), m_pool, m_back);
  m_slots[m_back].fence_counter = g_command_buffer_mgr->GetCurrentFenceCounter();
  m_back = (m_back + 1) % RING_SIZE;
  m_query_count.fetch_add(1, std::memory_order_relaxed);
}

void PerfQuery::ResetQuery()
{
  m_query_count.store(0, std::memory_order_relaxed);
  m_front = 0;
  m_back = 0;
  for (std::atomic<u32>& result : m_results)
    result.store(0, std::memory_order_relaxed);

  StateTracker::GetInstance()->EndRenderPass();
  vkCmdResetQueryPool(Cmd(), m_pool, 0, RING_SIZE);
  m_slots.fill({});
}

u32 PerfQuery::GetQueryResult(PerfQueryType type)
{
  const auto value = [this](PerfQueryGroup group) {
    return m_results[group].load(std::memory_order_relaxed);
  };

  u32 total = 0;
  switch (type)
  {
  case PQ_ZCOMP_INPUT_ZCOMPLOC:
  case PQ_ZCOMP_OUTPUT_ZCOMPLOC:
    total = value(PQG_ZCOMP_ZCOMPLOC);
    break;
  case PQ_ZCOMP_INPUT:
  case PQ_ZCOMP_OUTPUT:
    total = value(PQG_ZCOMP);
    break;
  case PQ_BLEND_INPUT:
    total = value(PQG_ZCOMP) + value(PQG_ZCOMP_ZCOMPLOC);
    break;
  case PQ_EFB_COPY_CLOCKS:
    total = value(PQG_EFB_COPY_CLOCKS);
    break;
  default:
    break;
  }
  // The counters advance once per 2x2 quad.
  return total / 4;
}

void PerfQuery::FlushResults()
{
  if (!IsFlushed())
    PartialFlush(true);
  ASSERT(IsFlushed());
}

bool PerfQuery::IsFlushed() const
{
  return m_query_count.load(std::memory_order_relaxed) == 0;
}

void PerfQuery::CollectFinished()
{
  const u64 completed = g_command_buffer_mgr->GetCompletedFenceCounter();
  const u32 pending = m_query_count.load(std::memory_order_relaxed);

  // Walk the finished prefix of the ring, collecting in two runs if it wraps.
  u32 run = 0;
  for (u32 seen = 0; seen < pending; ++seen)
  {
    const u32 slot = (m_front + run) % RING_SIZE;
    if (m_slots[slot].fence_counter > completed)
      break;
    if (slot < m_front)
    {
      // Wrapped: read out the part up to the end of the ring first.
      Collect(run);
      DEBUG_ASSERT(m_front == 0);
      run = 0;
    }
    ++run;
  }
  if (run != 0)
    Collect(run);
}

void PerfQuery::Collect(u32 count)
{
  ASSERT(count <= m_query_count.load(std::memory_order_relaxed) && m_front + count <= RING_SIZE);

  const VkResult res = vkGetQueryPoolResults(
      g_vulkan_context->GetDevice(), m_pool, m_front, count, count * sizeof(u32), m_samples.data(),
      sizeof(u32), VK_QUERY_RESULT_WAIT_BIT);
  if (res != VK_SUCCESS)
    LOG_VULKAN_ERROR(res, "vkGetQueryPoolResults failed: ");

  // Make the slots reusable.
  StateTracker::GetInstance()->EndRenderPass();
  vkCmdResetQueryPool(Cmd(), m_pool, m_front, count);

  for (u32 n = 0; n < count; ++n)
  {
    Slot& slot = m_slots[(m_front + n) % RING_SIZE];
    DEBUG_ASSERT(slot.fence_counter != 0);
    slot.fence_counter = 0;
    slot.in_use = false;
    m_results[slot.group].fetch_add(ToNativePixels(m_samples[n]), std::memory_order_relaxed);
  }

  m_front = (m_front + count) % RING_SIZE;
  m_query_count.fetch_sub(count, std::memory_order_relaxed);
}

void PerfQuery::PartialFlush(bool blocking)
{
  // The oldest query may still be in the command buffer being recorded: submit it (in the
  // background unless blocking).
  if (blocking ||
      m_slots[m_front].fence_counter == g_command_buffer_mgr->GetCurrentFenceCounter())
  {
    VKGfx::GetInstance()->ExecuteCommandBuffer(true, blocking);
  }
  CollectFinished();
}
}  // namespace Vulkan
