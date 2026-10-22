// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "VirtualHeap.h"

namespace
{
	constexpr uint32 kAllocGranularity = 256;
	constexpr size_t kEntriesPerBatch = 16 * 1024;

	uint32 roundUp(uint32 v, uint32 granularity)
	{
		return (v + granularity - 1) & ~(granularity - 1);
	}

	// adds a batch of entry records to the heap's recycle list
	void addEntryBatch(VirtualBufferHeap_t* heap)
	{
		auto* batch = static_cast<VirtualBufferHeapEntry_t*>(malloc(sizeof(VirtualBufferHeapEntry_t) * kEntriesPerBatch));
		if (!batch)
			return;
		for (size_t i = kEntriesPerBatch; i-- > 0;)
		{
			batch[i].next = heap->firstUnusedEntry;
			heap->firstUnusedEntry = batch + i;
		}
	}

	VirtualBufferHeapEntry_t* takeEntry(VirtualBufferHeap_t* heap)
	{
		if (!heap->firstUnusedEntry)
		{
			cemuLog_log(LogType::Force, "virtualBufferHeap: entry pool exhausted, growing it");
			addEntryBatch(heap);
			if (!heap->firstUnusedEntry)
				return nullptr;
		}
		VirtualBufferHeapEntry_t* e = heap->firstUnusedEntry;
		heap->firstUnusedEntry = e->next;
		e->next = nullptr;
		e->previous = nullptr;
		return e;
	}

	void recycleEntry(VirtualBufferHeap_t* heap, VirtualBufferHeapEntry_t* e)
	{
		e->next = heap->firstUnusedEntry;
		heap->firstUnusedEntry = e;
	}
}

VirtualBufferHeap_t* virtualBufferHeap_create(uint32 virtualHeapSize, void* baseAddr)
{
	auto* heap = static_cast<VirtualBufferHeap_t*>(calloc(1, sizeof(VirtualBufferHeap_t)));
	heap->virtualSize = roundUp(virtualHeapSize, 32);
	heap->baseAddress = baseAddr;
	addEntryBatch(heap);
	return heap;
}

VirtualBufferHeapEntry_t* virtualBufferHeap_allocate(VirtualBufferHeap_t* bufferHeap, uint32 size)
{
	size = roundUp(size, kAllocGranularity);
	// walk the sorted entries; candidate is the first offset after the previous entry
	VirtualBufferHeapEntry_t* before = nullptr;
	VirtualBufferHeapEntry_t* after = bufferHeap->firstEntry;
	uint32 candidate = 0;
	while (after && candidate + size > after->startOffset)
	{
		candidate = roundUp(after->endOffset, kAllocGranularity);
		before = after;
		after = after->next;
	}
	if (candidate + size > bufferHeap->virtualSize)
		return nullptr;
	VirtualBufferHeapEntry_t* e = takeEntry(bufferHeap);
	if (!e)
		return nullptr;
	e->startOffset = candidate;
	e->endOffset = candidate + size;
	e->previous = before;
	e->next = after;
	if (before)
		before->next = e;
	else
		bufferHeap->firstEntry = e;
	if (after)
		after->previous = e;
	bufferHeap->stats.allocatedMemory += size;
	bufferHeap->stats.numActiveAllocs++;
	return e;
}

void virtualBufferHeap_free(VirtualBufferHeap_t* bufferHeap, VirtualBufferHeapEntry_t* entry)
{
	if (entry->previous)
		entry->previous->next = entry->next;
	else
		bufferHeap->firstEntry = entry->next;
	if (entry->next)
		entry->next->previous = entry->previous;
	bufferHeap->stats.allocatedMemory -= entry->endOffset - entry->startOffset;
	bufferHeap->stats.numActiveAllocs--;
	recycleEntry(bufferHeap, entry);
}

void* virtualBufferHeap_allocateAddr(VirtualBufferHeap_t* bufferHeap, uint32 size)
{
	VirtualBufferHeapEntry_t* e = virtualBufferHeap_allocate(bufferHeap, size);
	if (!e)
		return nullptr;
	return static_cast<uint8*>(bufferHeap->baseAddress) + e->startOffset;
}

void virtualBufferHeap_freeAddr(VirtualBufferHeap_t* bufferHeap, void* addr)
{
	for (VirtualBufferHeapEntry_t* e = bufferHeap->firstEntry; e; e = e->next)
	{
		if (static_cast<uint8*>(bufferHeap->baseAddress) + e->startOffset == static_cast<uint8*>(addr))
		{
			virtualBufferHeap_free(bufferHeap, e);
			return;
		}
	}
	cemu_assert_suspicious();
}
