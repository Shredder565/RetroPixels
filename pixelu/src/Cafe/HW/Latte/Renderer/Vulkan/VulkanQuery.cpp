// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"

// An occlusion query can stay open across command buffers, but a Vulkan query cannot. Every command
// buffer the guest query spans gets its own Vulkan query ("segment"); each segment's result is copied
// into the host visible result buffer, and the guest result is the sum over all segments.
class LatteQueryObjectVk : public LatteQueryObject
{
	friend class VulkanRenderer;

	LatteQueryObjectVk(VulkanRenderer* rendererVk) : m_renderer(rendererVk) {}

	bool getResult(uint64& numSamplesPassed) override;
	void begin() override;
	void end() override;

	void OpenSegment();
	void CloseSegment();
	void CollectFinishedSegments();

	uint32 TakePoolSlot();
	void ReturnPoolSlot(uint32 slot);

	struct Segment
	{
		uint32 poolSlot;
		uint64 commandBufferId; // command buffer that copies the result, valid once closed
		bool closed;
	};

	VulkanRenderer* m_renderer;
	std::vector<Segment> m_segments; // oldest first
	bool m_ended{}; // end() was called, result available once m_endCommandBuffer finished
	bool m_hasActiveQuery{};
	bool m_segmentOpen{};
	uint64 m_endCommandBuffer{};
	uint64 m_samples{};
};

bool LatteQueryObjectVk::getResult(uint64& numSamplesPassed)
{
	if (!m_ended || !m_renderer->HasCommandBufferFinished(m_endCommandBuffer))
		return false;
	CollectFinishedSegments();
	cemu_assert_debug(m_segments.empty());
	numSamplesPassed = m_samples;
	return true;
}

void LatteQueryObjectVk::begin()
{
	m_ended = false;
	m_hasActiveQuery = true;
	OpenSegment();
}

void LatteQueryObjectVk::end()
{
	cemu_assert_debug(!m_segments.empty());
	if (m_segmentOpen)
		CloseSegment();
	m_ended = true;
	m_hasActiveQuery = false;
	m_endCommandBuffer = m_renderer->GetCurrentCommandBufferId();
	m_renderer->m_occlusionQueries.m_lastCommandBuffer = m_endCommandBuffer;
	// the guest usually waits for the result, so get the command buffer submitted early
	m_renderer->RequestSubmitSoon();
	m_renderer->RequestSubmitOnIdle();
}

void LatteQueryObjectVk::OpenSegment()
{
	m_renderer->draw_endRenderPass(); // queries are begun outside of render passes
	CollectFinishedSegments();

	const uint32 slot = TakePoolSlot();
	m_segments.push_back(Segment{ slot, 0, false });
	VkCommandBuffer cmd = m_renderer->m_state.currentCommandBuffer;
	VkQueryPool pool = m_renderer->m_occlusionQueries.queryPool;
	vkCmdResetQueryPool(cmd, pool, slot, 1);
	vkCmdBeginQuery(cmd, pool, slot, VK_QUERY_CONTROL_PRECISE_BIT);
	m_segmentOpen = true;
}

void LatteQueryObjectVk::CloseSegment()
{
	m_renderer->draw_endRenderPass();
	cemu_assert_debug(m_segmentOpen);

	Segment& seg = m_segments.back();
	VkCommandBuffer cmd = m_renderer->m_state.currentCommandBuffer;
	auto& queries = m_renderer->m_occlusionQueries;
	vkCmdEndQuery(cmd, queries.queryPool, seg.poolSlot);
	// each pool slot has its own 64-bit result entry in the buffer
	vkCmdCopyQueryPoolResults(cmd, queries.queryPool, seg.poolSlot, 1, queries.bufferQueryResults, seg.poolSlot * sizeof(uint64), sizeof(uint64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
	seg.commandBufferId = m_renderer->GetCurrentCommandBufferId();
	seg.closed = true;
	m_segmentOpen = false;
}

void LatteQueryObjectVk::CollectFinishedSegments()
{
	// segments finish in order, stop at the first one that is still pending
	size_t done = 0;
	for (; done < m_segments.size(); done++)
	{
		const Segment& seg = m_segments[done];
		if (!seg.closed || !m_renderer->HasCommandBufferFinished(seg.commandBufferId))
			break;
		m_samples += m_renderer->m_occlusionQueries.ptrQueryResults[seg.poolSlot];
		ReturnPoolSlot(seg.poolSlot);
	}
	m_segments.erase(m_segments.begin(), m_segments.begin() + done);
}

uint32 LatteQueryObjectVk::TakePoolSlot()
{
	auto& freeSlots = m_renderer->m_occlusionQueries.list_availableQueryIndices;
	if (freeSlots.empty())
	{
		cemuLog_log(LogType::Force, "Vulkan-Error: Exhausted query pool");
		assert_dbg();
	}
	const uint32 slot = freeSlots.back();
	freeSlots.pop_back();
	return slot;
}

void LatteQueryObjectVk::ReturnPoolSlot(uint32 slot)
{
	m_renderer->m_occlusionQueries.list_availableQueryIndices.emplace_back(slot);
}

LatteQueryObject* VulkanRenderer::occlusionQuery_create()
{
	auto& queries = m_occlusionQueries;
	// the pool is made on first use
	if (queries.queryPool == VK_NULL_HANDLE)
	{
		VkQueryPoolCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		info.queryType = VK_QUERY_TYPE_OCCLUSION;
		info.queryCount = OCCLUSION_QUERY_POOL_SIZE;
		VkResult res = vkCreateQueryPool(m_logicalDevice, &info, nullptr, &queries.queryPool);
		if (res != VK_SUCCESS)
		{
			cemuLog_log(LogType::Force, "Vulkan-Error: Failed to create query pool with error {}", (sint32)res);
			return nullptr;
		}
	}

	// reuse released query objects, oldest first
	LatteQueryObjectVk* query;
	if (queries.list_cachedQueries.empty())
	{
		query = new LatteQueryObjectVk(this);
	}
	else
	{
		query = queries.list_cachedQueries.front();
		queries.list_cachedQueries.erase(queries.list_cachedQueries.begin());
	}
	query->queryEnded = false;
	query->queryEventStart = 0;
	query->queryEventEnd = 0;
	query->m_ended = false;
	query->m_samples = 0;
	cemu_assert_debug(query->m_segments.empty()); // occlusionQuery_destroy() empties it
	queries.list_currentlyActiveQueries.emplace_back(query);
	return query;
}

void VulkanRenderer::occlusionQuery_destroy(LatteQueryObject* queryObj)
{
	auto* query = static_cast<LatteQueryObjectVk*>(queryObj);
	auto& active = m_occlusionQueries.list_currentlyActiveQueries;
	active.erase(std::remove(active.begin(), active.end(), query), active.end());
	m_occlusionQueries.list_cachedQueries.emplace_back(query);
	// pending segments are dropped, their pool slots become free again
	for (const auto& seg : query->m_segments)
		query->ReturnPoolSlot(seg.poolSlot);
	query->m_segments.clear();
}

void VulkanRenderer::occlusionQuery_flush()
{
	WaitCommandBufferFinished(m_occlusionQueries.m_lastCommandBuffer);
}

void VulkanRenderer::occlusionQuery_updateState()
{
	// segment results become available as command buffers finish
	ProcessFinishedCommandBuffers();
}

void VulkanRenderer::occlusionQuery_notifyEndCommandBuffer()
{
	for (LatteQueryObjectVk* query : m_occlusionQueries.list_currentlyActiveQueries)
	{
		if (query->m_hasActiveQuery)
			query->CloseSegment();
	}
}

void VulkanRenderer::occlusionQuery_notifyBeginCommandBuffer()
{
	for (LatteQueryObjectVk* query : m_occlusionQueries.list_currentlyActiveQueries)
	{
		if (query->m_hasActiveQuery)
			query->OpenSegment();
	}
}
