// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_BlockHeap.h"

/* Block heap
 * The managed range is split into consecutive tracks (free or used) linked in address order.
 * Splitting a free track needs spare track structs, which the application provides through
 * MEMAddBlockHeapTracking; spares are kept on a singly linked list through nextBlock.
 * Allocations are zero-filled. The track lists are protected by the scheduler lock. */

namespace coreinit
{
	namespace
	{
		using Track = MEMBlockHeapTrack;

		bool IsBlockHeap(MEMBlockHeap* heap)
		{
			return heap && heap->magic == MEMHeapMagic::BLOCK_HEAP;
		}

		Track* TakeSpareTrack(MEMBlockHeap* heap)
		{
			Track* spare = heap->nextFreeBlock.GetPtr();
			heap->nextFreeBlock = spare->nextBlock;
			spare->nextBlock = nullptr;
			if (heap->freeBlocksLeft == 0)
			{
				cemuLog_log(LogType::Force, "BlockHeap: out of spare tracks");
				assert_dbg();
			}
			heap->freeBlocksLeft = heap->freeBlocksLeft - 1;
			return spare;
		}

		void ReturnSpareTrack(MEMBlockHeap* heap, Track* track)
		{
			track->nextBlock = heap->nextFreeBlock;
			heap->nextFreeBlock = track;
			heap->freeBlocksLeft = heap->freeBlocksLeft + 1;
		}

		// marks [addr, addr + size) of a free track as used, splitting off the rest
		sint32 Occupy(MEMBlockHeap* heap, Track* track, uint32 addr, uint32 size)
		{
			const uint32 trackStart = track->addrStart;
			const uint32 trackLast = track->addrEnd;
			const bool needFront = addr != trackStart;
			const bool needBack = (trackLast + 1) != (addr + size);
			if (needFront && needBack)
			{
				if (heap->freeBlocksLeft < 2)
					return -1;
			}
			else if (needFront || needBack)
			{
				if (heap->freeBlocksLeft < 1)
					return -2;
			}

			Track* before = track->previousBlock.GetPtr();
			if (needFront)
			{
				Track* front = TakeSpareTrack(heap);
				front->isFree = 1;
				front->addrStart = trackStart;
				front->addrEnd = addr - 1;
				if (heap->headBlock.GetPtr() == track)
					heap->headBlock = front;
				if (before)
					before->nextBlock = front;
				front->nextBlock = track;
				front->previousBlock = before;
				before = front;
			}
			track->isFree = 0;
			track->addrStart = addr;
			track->addrEnd = addr + size - 1;
			track->previousBlock = before;
			if (needBack)
			{
				Track* back = TakeSpareTrack(heap);
				Track* after = track->nextBlock.GetPtr();
				back->isFree = 1;
				back->addrStart = addr + size;
				back->addrEnd = trackLast;
				back->previousBlock = track;
				back->nextBlock = after;
				if (heap->tailBlock.GetPtr() == track)
					heap->tailBlock = back;
				if (after)
					after->previousBlock = back;
				track->nextBlock = back;
			}
			memset(MEMPTR<uint8>(addr).GetPtr(), 0, size);
			return 0;
		}

		Track* TrackStartingAt(MEMBlockHeap* heap, uint32 addr)
		{
			if (addr < heap->heapStart.GetMPTR() || addr > heap->heapEnd.GetMPTR())
				return nullptr;
			for (Track* t = heap->headBlock.GetPtr(); t; t = t->nextBlock.GetPtr())
			{
				if (t->addrStart == addr)
					return t;
			}
			return nullptr;
		}
	}

	MEMHeapHandle MEMInitBlockHeap(MEMBlockHeap* memStart, void* startAddr, void* endAddr, void* initTrackMem, uint32 initTrackMemSize, uint32 createFlags)
	{
		if (!memStart || !startAddr || !endAddr)
			return nullptr;
		const uint32 start = MEMPTR<void>(startAddr).GetMPTR();
		const uint32 end = MEMPTR<void>(endAddr).GetMPTR();
		if (start >= end)
			return nullptr;
		if (initTrackMemSize == 0)
			initTrackMem = nullptr;
		else if (!initTrackMem)
			initTrackMemSize = 0;

		MEMBlockHeap* heap = memStart;
		MEMInitHeapBase(heap, MEMHeapMagic::BLOCK_HEAP, startAddr, endAddr, createFlags);
		heap->track.addrStart = start;
		heap->track.addrEnd = end - 1;
		heap->track.isFree = 1;
		heap->track.previousBlock = nullptr;
		heap->track.nextBlock = nullptr;
		heap->headBlock = &heap->track;
		heap->tailBlock = &heap->track;
		heap->nextFreeBlock = nullptr;
		heap->freeBlocksLeft = 0;
		cemu_assert_debug(!heap->HasOptionFill()); // fill is not supported

		if (initTrackMemSize != 0 && MEMAddBlockHeapTracking(heap, initTrackMem, initTrackMemSize) != 0)
		{
			MEMBaseDestroyHeap(heap);
			return nullptr;
		}
		return heap;
	}

	void* MEMDestroyBlockHeap(MEMHeapHandle hHeap)
	{
		if (!hHeap)
			return nullptr;
		cemu_assert_debug(hHeap->magic == MEMHeapMagic::BLOCK_HEAP);
		MEMBaseDestroyHeap(hHeap);
		memset(hHeap, 0, sizeof(MEMBlockHeap));
		return hHeap;
	}

	// largest free track size after aligning its start (alignment need not be a power of two here)
	sint32 MEMGetAllocatableSizeForBlockHeapEx(MEMBlockHeap* blockHeap, sint32 alignment)
	{
		if (!IsBlockHeap(blockHeap))
		{
			cemuLog_log(LogType::Force, "MEMGetAllocatableSizeForBlockHeapEx(): not a block heap");
			return 0;
		}
		uint32 align = (uint32)alignment;
		if (alignment < 0)
			align = 0u - align;
		else if (alignment == 0)
			align = 4;
		blockHeap->AcquireLock();
		uint32 best = 0;
		for (Track* t = blockHeap->headBlock.GetPtr(); t; t = t->nextBlock.GetPtr())
		{
			if (t->isFree == 0)
				continue;
			const uint32 last = t->addrEnd;
			const uint32 alignedStart = ((t->addrStart + align - 1) / align) * align;
			if (alignedStart <= last)
				best = std::max(best, (last + 1) - alignedStart);
		}
		blockHeap->ReleaseLock();
		return best;
	}

	// hands a chunk of memory to the heap to be used as spare tracks; returns 0 or -4 if it is too small
	sint32 MEMAddBlockHeapTracking(MEMBlockHeap* heap, void* trackMem, uint32 trackMemSize)
	{
		if (!IsBlockHeap(heap))
			return 0;
		if (!trackMem || trackMemSize <= sizeof(MEMBlockHeapTrackGroup) + sizeof(Track))
			return -4;
		const uint32 count = (trackMemSize - sizeof(MEMBlockHeapTrackGroup)) / sizeof(Track);
		auto* group = (MEMBlockHeapTrackGroup*)trackMem;
		Track* tracks = (Track*)(group + 1);
		group->_ukn00 = 0;
		group->_ukn04 = 0;
		group->tracks = tracks;
		group->trackCount = count;
		tracks[0].previousBlock = nullptr;
		for (uint32 i = 0; i + 1 < count; i++)
		{
			tracks[i].nextBlock = &tracks[i + 1];
			tracks[i + 1].previousBlock = nullptr;
		}
		__OSLockScheduler();
		tracks[count - 1].nextBlock = heap->nextFreeBlock;
		heap->nextFreeBlock = &tracks[0];
		heap->freeBlocksLeft = heap->freeBlocksLeft + count;
		__OSUnlockScheduler();
		return 0;
	}

	uint32 MEMGetTrackingLeftInBlockHeap(MEMBlockHeap* heap)
	{
		if (!IsBlockHeap(heap))
			return 0;
		return heap->freeBlocksLeft;
	}

	// positive alignment: lowest fitting address, negative alignment: highest fitting address
	MPTR MEMAllocFromBlockHeapEx(MEMBlockHeap* heap, uint32 size, sint32 alignment)
	{
		if (!IsBlockHeap(heap))
			return MPTR_NULL;
		__OSLockScheduler();
		const bool fromTail = alignment < 0;
		uint32 align = (uint32)alignment;
		if (alignment == 0)
			align = 4;
		else if (fromTail)
			align = 0u - align;
		cemu_assert_debug(std::popcount(align) == 1);
		const uint32 mask = ~(align - 1);

		Track* t = fromTail ? heap->tailBlock.GetPtr() : heap->headBlock.GetPtr();
		for (; t; t = fromTail ? t->previousBlock.GetPtr() : t->nextBlock.GetPtr())
		{
			if (t->isFree == 0)
				continue;
			const uint32 rangeStart = t->addrStart;
			const uint32 rangeEnd = t->addrEnd + 1;
			uint32 addr;
			if (fromTail)
			{
				if (size > rangeEnd - rangeStart)
					continue;
				addr = (rangeEnd - size) & mask;
				if (addr < rangeStart)
					continue;
			}
			else
			{
				addr = (rangeStart + align - 1) & mask;
				if (addr >= rangeEnd || rangeEnd - addr < size)
					continue;
			}
			// the first fitting track decides the result, even if it runs out of spare tracks
			const sint32 err = Occupy(heap, t, addr, size);
			__OSUnlockScheduler();
			return err == 0 ? addr : MPTR_NULL;
		}
		__OSUnlockScheduler();
		return MPTR_NULL;
	}

	void MEMFreeToBlockHeap(MEMBlockHeap* heap, MPTR memAddr)
	{
		if (!IsBlockHeap(heap))
			return;
		__OSLockScheduler();
		Track* t = TrackStartingAt(heap, memAddr);
		if (t)
		{
			t->isFree = 1;
			Track* before = t->previousBlock.GetPtr();
			if (before && before->isFree != 0)
			{
				// fold into the free track in front
				Track* after = t->nextBlock.GetPtr();
				before->addrEnd = t->addrEnd;
				before->nextBlock = after;
				if (after)
					after->previousBlock = before;
				if (heap->tailBlock.GetPtr() == t)
					heap->tailBlock = before;
				ReturnSpareTrack(heap, t);
				t = before;
			}
			Track* after = t->nextBlock.GetPtr();
			if (after && after->isFree != 0)
			{
				// absorb the free track behind
				Track* afterNext = after->nextBlock.GetPtr();
				t->addrEnd = after->addrEnd;
				t->nextBlock = afterNext;
				if (afterNext)
					afterNext->previousBlock = t;
				if (heap->tailBlock.GetPtr() == after)
					heap->tailBlock = t;
				ReturnSpareTrack(heap, after);
			}
		}
		__OSUnlockScheduler();
	}

	uint32 MEMGetTotalFreeSizeForBlockHeap(MEMBlockHeap* blockHeap)
	{
		if (!IsBlockHeap(blockHeap))
			return 0;
		__OSLockScheduler();
		uint32 total = 0;
		for (Track* t = blockHeap->headBlock.GetPtr(); t; t = t->nextBlock.GetPtr())
		{
			if (t->isFree != 0)
				total += t->addrEnd - t->addrStart + 1;
		}
		__OSUnlockScheduler();
		return total;
	}

	void InitializeMEMBlockHeap()
	{
		cafeExportRegister("coreinit", MEMInitBlockHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMDestroyBlockHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetAllocatableSizeForBlockHeapEx, LogType::CoreinitMem);

		cafeExportRegister("coreinit", MEMAddBlockHeapTracking, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetTrackingLeftInBlockHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMAllocFromBlockHeapEx, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMFreeToBlockHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetTotalFreeSizeForBlockHeap, LogType::CoreinitMem);
	}
}
