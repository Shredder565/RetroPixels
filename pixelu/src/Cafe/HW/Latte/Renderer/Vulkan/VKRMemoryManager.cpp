// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Renderer/Vulkan/VKRMemoryManager.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include <imgui.h>

namespace
{
	VkBufferUsageFlags UsageForBufferType(VKR_BUFFER_TYPE type)
	{
		switch (type)
		{
		case VKR_BUFFER_TYPE::STAGING: return VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		case VKR_BUFFER_TYPE::INDEX: return VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
		case VKR_BUFFER_TYPE::STRIDE: return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		}
		cemu_assert_debug(false);
		return 0;
	}

	VkDevice Device()
	{
		return VulkanRenderer::GetInstance()->GetLogicalDevice();
	}

	void FlushRange(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size)
	{
		VkMappedMemoryRange range{};
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = memory;
		range.offset = offset;
		range.size = size;
		vkFlushMappedMemoryRanges(device, 1, &range);
	}

	constexpr uint32 kRingGranularity = 128; // ring allocations are multiples of this and at least this aligned
	constexpr uint32 kIdleCleanupsBeforeRelease = 1000;
}

// ---- VKRSynchronizedRingAllocator ----

VKRSynchronizedRingAllocator::~VKRSynchronizedRingAllocator()
{
	for (AllocatorBuffer_t& buffer : m_buffers)
		m_vkrMemMgr->DeleteBuffer(buffer.vk_buffer, buffer.vk_mem);
}

void VKRSynchronizedRingAllocator::addUploadBufferSyncPoint(AllocatorBuffer_t& buffer, uint32 offset)
{
	// only the first allocation of each command buffer is recorded
	const uint64 current = m_vkr->GetCurrentCommandBufferId();
	if (buffer.lastSyncpointCmdBufferId == current)
		return;
	buffer.lastSyncpointCmdBufferId = current;
	buffer.queue_syncPoints.emplace(current, offset);
}

void VKRSynchronizedRingAllocator::allocateAdditionalUploadBuffer(uint32 sizeRequiredForAlloc)
{
	// buffers grow in steps of the minimum size
	const uint32 steps = std::max<uint32>(1, (sizeRequiredForAlloc + m_minimumBufferAllocSize - 1) / m_minimumBufferAllocSize);
	const uint32 bufferSize = steps * m_minimumBufferAllocSize;

	const VkMemoryPropertyFlags props = m_bufferType == VKR_BUFFER_TYPE::STAGING ?
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : // flushed explicitly
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	AllocatorBuffer_t buffer{};
	m_vkrMemMgr->CreateBuffer(bufferSize, UsageForBufferType(m_bufferType), props, buffer.vk_buffer, buffer.vk_mem);
	void* mapped = nullptr;
	vkMapMemory(m_vkr->GetLogicalDevice(), buffer.vk_mem, 0, VK_WHOLE_SIZE, 0, &mapped);
	buffer.basePtr = (uint8*)mapped;
	buffer.size = bufferSize;
	buffer.writeIndex = 0;
	buffer.index = (uint32)m_buffers.size();
	m_buffers.push_back(std::move(buffer));
}

bool VKRSynchronizedRingAllocator::TryReserve(AllocatorBuffer_t& buffer, uint32 size, uint32 alignment, AllocatorReservation_t& out)
{
	const bool inFlight = !buffer.queue_syncPoints.empty();
	const uint32 oldestInUse = inFlight ? buffer.queue_syncPoints.front().offset : 0;

	uint32 start = buffer.writeIndex;
	uint32 padding = (alignment - start % alignment) % alignment;
	// free space ahead of the write position; unlimited when the in-use region lies behind it
	const bool limited = inFlight && oldestInUse >= start;
	if (limited && padding + size > oldestInUse - start)
		return false;
	if (start + padding + size > buffer.size)
	{
		// continue at the start of the buffer (offset 0 is always aligned)
		if (inFlight ? size > oldestInUse : size > buffer.size)
			return false;
		start = 0;
		padding = 0;
		buffer.writeIndex = 0;
	}
	addUploadBufferSyncPoint(buffer, start);
	const uint32 offset = start + padding;
	buffer.writeIndex = offset + size;
	buffer.cleanupCounter = 0;

	out.vkBuffer = buffer.vk_buffer;
	out.vkMem = buffer.vk_mem;
	out.memPtr = buffer.basePtr + offset;
	out.bufferOffset = offset;
	out.size = size;
	out.bufferIndex = buffer.index;
	return true;
}

VKRSynchronizedRingAllocator::AllocatorReservation_t VKRSynchronizedRingAllocator::AllocateBufferMemory(uint32 size, uint32 alignment)
{
	alignment = std::max(alignment, kRingGranularity);
	size = (size + kRingGranularity - 1) & ~(kRingGranularity - 1);
	AllocatorReservation_t reservation;
	while (true)
	{
		for (AllocatorBuffer_t& buffer : m_buffers)
		{
			if (TryReserve(buffer, size, alignment, reservation))
				return reservation;
		}
		// every buffer is full up to its oldest in-flight region
		allocateAdditionalUploadBuffer(size);
	}
}

void VKRSynchronizedRingAllocator::FlushReservation(AllocatorReservation_t& uploadReservation)
{
	cemu_assert_debug(m_bufferType == VKR_BUFFER_TYPE::STAGING); // the other ring types use coherent memory
	FlushRange(m_vkr->GetLogicalDevice(), uploadReservation.vkMem, uploadReservation.bufferOffset, uploadReservation.size);
}

void VKRSynchronizedRingAllocator::CleanupBuffer(uint64 latestFinishedCommandBufferId)
{
	// sync points strictly older than this id are released
	const uint64 releaseBefore = latestFinishedCommandBufferId > 1 ? latestFinishedCommandBufferId - 1 : latestFinishedCommandBufferId;
	for (AllocatorBuffer_t& buffer : m_buffers)
	{
		auto& points = buffer.queue_syncPoints;
		while (!points.empty() && points.front().commandBufferId < releaseBefore)
			points.pop();
		if (points.empty())
			buffer.cleanupCounter++;
	}

	// give back the newest buffer once it has been idle for a long time (the first one always stays)
	if (m_buffers.size() < 2)
		return;
	AllocatorBuffer_t& newest = m_buffers.back();
	if (newest.cleanupCounter < kIdleCleanupsBeforeRelease)
		return;
	vkUnmapMemory(m_vkr->GetLogicalDevice(), newest.vk_mem);
	m_vkrMemMgr->DeleteBuffer(newest.vk_buffer, newest.vk_mem);
	m_buffers.pop_back();
}

VkBuffer VKRSynchronizedRingAllocator::GetBufferByIndex(uint32 index) const
{
	return m_buffers[index].vk_buffer;
}

void VKRSynchronizedRingAllocator::GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const
{
	numBuffers = (uint32)m_buffers.size();
	totalBufferSize = 0;
	freeBufferSize = 0;
	for (const AllocatorBuffer_t& buffer : m_buffers)
	{
		totalBufferSize += buffer.size;
		uint32 available = buffer.size;
		if (!buffer.queue_syncPoints.empty())
		{
			const uint32 oldest = buffer.queue_syncPoints.front().offset;
			// space from the write position up to the oldest in-flight data, wrapping at the end
			available = oldest >= buffer.writeIndex ? oldest - buffer.writeIndex : buffer.size - buffer.writeIndex + oldest;
		}
		freeBufferSize += available;
	}
}

// ---- VKRSynchronizedHeapAllocator ----

VKRSynchronizedHeapAllocator::VKRSynchronizedHeapAllocator(class VKRMemoryManager* vkMemoryManager, VKR_BUFFER_TYPE bufferType, size_t minimumBufferAllocSize)
	: m_vkrMemMgr(vkMemoryManager), m_chunkedHeap(bufferType, minimumBufferAllocSize)
{
}

VKRSynchronizedHeapAllocator::AllocatorReservation* VKRSynchronizedHeapAllocator::AllocateBufferMemory(uint32 size, uint32 alignment)
{
	const CHAddr addr = m_chunkedHeap.alloc(size, alignment);
	m_activeAllocations.emplace_back(addr);
	AllocatorReservation* reservation = m_poolAllocatorReservation.allocObj();
	reservation->bufferIndex = addr.chunkIndex;
	reservation->bufferOffset = addr.offset;
	reservation->size = size;
	reservation->memPtr = m_chunkedHeap.GetChunkPtr(addr.chunkIndex) + addr.offset;
	m_chunkedHeap.GetChunkVkMemInfo(addr.chunkIndex, reservation->vkBuffer, reservation->vkMem);
	return reservation;
}

void VKRSynchronizedHeapAllocator::FreeReservation(AllocatorReservation* uploadReservation)
{
	// the GPU may still read it during the current command buffer, so the free is deferred
	const uint64 current = VulkanRenderer::GetInstance()->GetCurrentCommandBufferId();
	const auto match = [uploadReservation](const TrackedAllocation& t) {
		return t.allocation.chunkIndex == uploadReservation->bufferIndex && t.allocation.offset == uploadReservation->bufferOffset;
	};
	auto it = std::find_if(m_activeAllocations.begin(), m_activeAllocations.end(), match);
	cemu_assert_debug(it != m_activeAllocations.end());
	m_releaseQueue[current].emplace_back(it->allocation);
	m_activeAllocations.erase(it);
	m_poolAllocatorReservation.freeObj(uploadReservation);
}

void VKRSynchronizedHeapAllocator::FlushReservation(AllocatorReservation* uploadReservation)
{
	if (!m_chunkedHeap.RequiresFlush(uploadReservation->bufferIndex))
		return;
	FlushRange(Device(), uploadReservation->vkMem, uploadReservation->bufferOffset, uploadReservation->size);
}

void VKRSynchronizedHeapAllocator::CleanupBuffer(uint64 latestFinishedCommandBufferId)
{
	for (auto it = m_releaseQueue.begin(); it != m_releaseQueue.end();)
	{
		if (it->first > latestFinishedCommandBufferId)
		{
			++it;
			continue;
		}
		for (const CHAddr& addr : it->second)
			m_chunkedHeap.free(addr);
		it = m_releaseQueue.erase(it);
	}
}

void VKRSynchronizedHeapAllocator::GetStats(uint32& numBuffers, size_t& totalBufferSize, size_t& freeBufferSize) const
{
	m_chunkedHeap.GetStats(numBuffers, totalBufferSize, freeBufferSize);
}

// ---- VkTextureChunkedHeap ----

VkTextureChunkedHeap::~VkTextureChunkedHeap()
{
	const VkDevice device = Device();
	for (const ChunkInfo& chunk : m_list_chunkInfo)
		vkFreeMemory(device, chunk.mem, nullptr);
}

uint32 VkTextureChunkedHeap::allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize)
{
	cemu_assert_debug(chunkIndex == m_list_chunkInfo.size());
	m_list_chunkInfo.emplace_back();

	constexpr uint32 granularity = 32 * 1024;
	const uint32 minimumSize = (minimumAllocationSize + granularity - 1) & ~(granularity - 1);
	// the first chunk is small so that rarely used memory types do not waste much
	uint32 chunkSize = std::max<uint32>(chunkIndex == 0 ? 16u * 1024 * 1024 : 128u * 1024 * 1024, minimumSize);

	// device local types first, then whatever else the filter allows
	const std::vector<uint32> deviceLocal = m_vkrMemoryManager->FindMemoryTypes(m_typeFilter, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	std::vector<uint32> others = m_vkrMemoryManager->FindMemoryTypes(m_typeFilter, 0);
	others.erase(std::remove_if(others.begin(), others.end(), [&deviceLocal](uint32 t) {
		return std::find(deviceLocal.begin(), deviceLocal.end(), t) != deviceLocal.end();
	}), others.end());
	std::vector<uint32> candidates = deviceLocal;
	candidates.insert(candidates.end(), others.begin(), others.end());

	const VkDevice device = Device();
	for (sint32 attempt = 0; attempt < 3; attempt++)
	{
		for (uint32 memoryType : candidates)
		{
			VkMemoryAllocateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			info.allocationSize = chunkSize;
			info.memoryTypeIndex = memoryType;
			VkDeviceMemory memory;
			if (vkAllocateMemory(device, &info, nullptr, &memory) == VK_SUCCESS)
			{
				m_list_chunkInfo[chunkIndex].mem = memory;
				return chunkSize;
			}
		}
		// out of memory: try a smaller chunk as long as it still fits the request
		chunkSize /= 2;
		if (chunkSize < minimumSize)
			break;
		cemuLog_log(LogType::Force, "Failed to allocate texture memory chunk with size {}MB. Trying again with smaller allocation size", chunkSize / 1024 / 1024);
	}
	cemuLog_log(LogType::Force, "Unable to allocate image memory chunk ({} heaps)", deviceLocal.size());
	throw std::runtime_error("failed to allocate image memory!");
}

// ---- VKRBuffer / VkBufferChunkedHeap ----

VKRBuffer* VKRBuffer::Create(VKR_BUFFER_TYPE bufferType, size_t bufferSize, VkMemoryPropertyFlags properties)
{
	VKRMemoryManager* memoryManager = VulkanRenderer::GetInstance()->GetMemoryManager();
	VkBuffer buffer;
	VkDeviceMemory memory;
	if (!memoryManager->CreateBuffer(bufferSize, UsageForBufferType(bufferType), properties, buffer, memory))
		return nullptr;

	VKRBuffer* result = new VKRBuffer(buffer, memory);
	if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
	{
		void* mapped = nullptr;
		vkMapMemory(Device(), memory, 0, bufferSize, 0, &mapped);
		result->m_mappedMemory = (uint8*)mapped;
		result->m_requiresFlush = !HAS_FLAG(properties, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	}
	return result;
}

VKRBuffer::~VKRBuffer()
{
	const VkDevice device = Device();
	if (m_mappedMemory)
		vkUnmapMemory(device, m_bufferMemory);
	if (m_bufferMemory != VK_NULL_HANDLE)
		vkFreeMemory(device, m_bufferMemory, nullptr);
	if (m_buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(device, m_buffer, nullptr);
}

VkBufferChunkedHeap::~VkBufferChunkedHeap()
{
	for (VKRBuffer* buffer : m_chunkBuffers)
		delete buffer;
}

uint32 VkBufferChunkedHeap::allocateNewChunk(uint32 chunkIndex, uint32 minimumAllocationSize)
{
	const size_t chunkSize = std::max<size_t>(m_minimumBufferAllocationSize, minimumAllocationSize);
	// prefer coherent memory, fall back to memory that needs explicit flushes
	VKRBuffer* buffer = VKRBuffer::Create(m_bufferType, chunkSize, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (!buffer)
		buffer = VKRBuffer::Create(m_bufferType, chunkSize, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	if (!buffer)
		VulkanRenderer::GetInstance()->UnrecoverableError("Failed to allocate buffer memory for VkBufferChunkedHeap");
	cemu_assert_debug(chunkIndex == m_chunkBuffers.size());
	m_chunkBuffers.emplace_back(buffer);
	return (uint32)chunkSize;
}

// ---- VKRMemoryManager ----

namespace
{
	template<typename TVisit>
	void ForEachMatchingMemoryType(VkPhysicalDevice physicalDevice, uint32 typeFilter, VkMemoryPropertyFlags properties, VkPhysicalDeviceMemoryProperties& props, TVisit visit)
	{
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
		for (uint32 t = 0; t < props.memoryTypeCount; t++)
		{
			const bool allowed = (typeFilter >> t) & 1;
			const bool hasProperties = (props.memoryTypes[t].propertyFlags & properties) == properties;
			if (allowed && hasProperties && !visit(t))
				return;
		}
	}
}

bool VKRMemoryManager::FindMemoryType(uint32 typeFilter, VkMemoryPropertyFlags properties, uint32& memoryIndex) const
{
	bool found = false;
	VkPhysicalDeviceMemoryProperties props;
	ForEachMatchingMemoryType(m_vkr->GetPhysicalDevice(), typeFilter, properties, props, [&](uint32 t) {
		memoryIndex = t;
		found = true;
		return false; // first match wins
	});
	return found;
}

std::vector<uint32> VKRMemoryManager::FindMemoryTypes(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
	std::vector<uint32> types;
	VkPhysicalDeviceMemoryProperties props;
	ForEachMatchingMemoryType(m_vkr->GetPhysicalDevice(), typeFilter, properties, props, [&](uint32 t) {
		types.push_back(t);
		return true;
	});
	if (types.empty())
		m_vkr->UnrecoverableError(fmt::format("Failed to find suitable memory type ({0:#08x} {1:#08x})", typeFilter, properties).c_str());
	return types;
}

size_t VKRMemoryManager::GetTotalMemoryForBufferType(VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, size_t minimumBufferSize)
{
	const VkDevice device = m_vkr->GetLogicalDevice();
	// a throwaway buffer tells which memory types such buffers may use (the size may matter)
	VkBufferCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = minimumBufferSize;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer probe;
	if (vkCreateBuffer(device, &info, nullptr, &probe) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Vulkan: GetTotalMemoryForBufferType() failed to create temporary buffer");
		return 0;
	}
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(device, probe, &req);
	vkDestroyBuffer(device, probe, nullptr);

	// add up every heap that backs at least one matching memory type, each heap once
	std::vector<uint32> heaps;
	VkPhysicalDeviceMemoryProperties props{};
	ForEachMatchingMemoryType(m_vkr->GetPhysicalDevice(), req.memoryTypeBits, properties, props, [&](uint32 t) {
		const uint32 heap = props.memoryTypes[t].heapIndex;
		if (std::find(heaps.begin(), heaps.end(), heap) == heaps.end())
			heaps.push_back(heap);
		return true;
	});
	size_t total = 0;
	for (uint32 heap : heaps)
	{
		if (heap > props.memoryHeapCount)
			continue;
		total += props.memoryHeaps[heap].size;
	}
	return total;
}

namespace
{
	// allocates memory for buffer and binds it; on failure the buffer is destroyed
	bool AllocateAndBind(const VKRMemoryManager& mm, VkDevice device, VkBuffer& buffer, VkMemoryPropertyFlags properties, const void* allocNext, VkDeviceMemory& memory, const char* bindFailMessage)
	{
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, buffer, &req);
		VkMemoryAllocateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		info.pNext = allocNext;
		info.allocationSize = req.size;
		if (!mm.FindMemoryType(req.memoryTypeBits, properties, info.memoryTypeIndex) ||
			vkAllocateMemory(device, &info, nullptr, &memory) != VK_SUCCESS)
		{
			vkDestroyBuffer(device, buffer, nullptr);
			return false;
		}
		if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS)
		{
			vkDestroyBuffer(device, buffer, nullptr);
			cemuLog_log(LogType::Force, "{}", bindFailMessage);
			return false;
		}
		return true;
	}
}

bool VKRMemoryManager::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) const
{
	const VkDevice device = m_vkr->GetLogicalDevice();
	VkBufferCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to create buffer (CreateBuffer)");
		return false;
	}
	return AllocateAndBind(*this, device, buffer, properties, nullptr, bufferMemory, "Failed to bind buffer (CreateBuffer)");
}

bool VKRMemoryManager::CreateBufferFromHostMemory(void* hostPointer, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) const
{
	const VkDevice device = m_vkr->GetLogicalDevice();
	VkExternalMemoryBufferCreateInfo external{};
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	VkBufferCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.pNext = &external;
	info.size = size;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS)
	{
		cemuLog_log(LogType::Force, "Failed to create buffer (CreateBuffer)");
		return false;
	}
	// the memory is the caller's allocation, imported as is
	VkImportMemoryHostPointerInfoEXT import{};
	import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	import.pHostPointer = hostPointer;
	return AllocateAndBind(*this, device, buffer, properties, &import, bufferMemory, "Failed to bind buffer (CreateBufferFromHostMemory)");
}

void VKRMemoryManager::DeleteBuffer(VkBuffer& buffer, VkDeviceMemory& deviceMem) const
{
	const VkDevice device = m_vkr->GetLogicalDevice();
	if (buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(device, buffer, nullptr);
	if (deviceMem != VK_NULL_HANDLE)
		vkFreeMemory(device, deviceMem, nullptr);
	buffer = VK_NULL_HANDLE;
	deviceMem = VK_NULL_HANDLE;
}

VkImageMemAllocation* VKRMemoryManager::imageMemoryAllocate(VkImage image)
{
	const VkDevice device = m_vkr->GetLogicalDevice();
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device, image, &req);
	const uint32 typeFilter = req.memoryTypeBits;
	const uint32 size = (uint32)req.size;
	const uint32 alignment = (uint32)req.alignment;

	std::unique_ptr<VkTextureChunkedHeap>& heapSlot = map_textureHeap[typeFilter];
	if (!heapSlot)
		heapSlot = std::make_unique<VkTextureChunkedHeap>(this, typeFilter);
	VkTextureChunkedHeap* heap = heapSlot.get();

	CHAddr mem = heap->allocMem(size, alignment);
	if (!mem.isValid())
	{
		// out of texture memory: evict unused textures twenty at a time until the allocation fits
		std::vector<LatteTexture*> evictable = LatteTC_GetDeleteableTextures();
		size_t next = 0;
		while (next < evictable.size() && !mem.isValid())
		{
			const size_t end = std::min(next + 20, evictable.size());
			for (; next < end; next++)
				LatteTexture_Delete(evictable[next]);
			mem = heap->allocMem(size, alignment);
		}
		if (!mem.isValid())
			m_vkr->UnrecoverableError("Ran out of VRAM for textures");
	}
	vkBindImageMemory(device, image, heap->getChunkMem(mem.chunkIndex), mem.offset);
	return new VkImageMemAllocation(typeFilter, mem, size);
}

void VKRMemoryManager::imageMemoryFree(VkImageMemAllocation* imageMemAllocation)
{
	const auto heap = map_textureHeap.find(imageMemAllocation->typeFilter);
	if (heap == map_textureHeap.end())
	{
		cemuLog_log(LogType::Force, "Internal texture heap error");
		return;
	}
	heap->second->freeMem(imageMemAllocation->mem);
	delete imageMemAllocation;
}

void VKRMemoryManager::appendOverlayHeapDebugInfo()
{
	for (const auto& [typeFilter, heap] : map_textureHeap)
	{
		uint32 heapBytes, usedBytes;
		heap->getStatistics(heapBytes, usedBytes);
		ImGui::Text("%s", fmt::format("{0:#08x} Size: {1}MB/{2}MB", typeFilter, usedBytes / 1024 / 1024, heapBytes / 1024 / 1024).c_str());
	}
}
