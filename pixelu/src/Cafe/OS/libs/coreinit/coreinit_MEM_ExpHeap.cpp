// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"

/* Expanded heap (MEMCreateExpHeapEx and friends)
 *
 * Every block, free or used, starts with a 0x14 byte header followed by its data. Free blocks
 * form a list sorted by address; used blocks form a list in allocation order. An allocation
 * splits a free block into [front gap][header + data][back gap]; gaps of at least 0x18 bytes
 * become free blocks again (the gap on the side the allocation is pushed against is only kept
 * when the heap has MEM_EXPHEAP_USE_ALIGN_MARGIN set), smaller gaps are absorbed by the used
 * block. The used block's attribute word remembers how many bytes in front of its header
 * belong to it, so freeing hands back the whole range.
 *
 * All address arithmetic is done on guest addresses widened to 64 bits so that sizes close
 * to 4 GiB and large alignments cannot wrap around. */

namespace coreinit
{
	struct MEMExpHeapBlock
	{
		/* +0x00 */ uint32be attribute; // bit 31: allocated from the tail, bits 8-30: leading gap, bits 0-7: group id
		/* +0x04 */ uint32be size;      // bytes of data after the header
		/* +0x08 */ MEMPTR<MEMExpHeapBlock> prev;
		/* +0x0C */ MEMPTR<MEMExpHeapBlock> next;
		/* +0x10 */ uint16be magic;
		/* +0x12 */ uint16be _pad;
	};
	static_assert(sizeof(MEMExpHeapBlock) == 0x14);

	namespace
	{
		constexpr uint32 HEADER_SIZE = sizeof(MEMExpHeapBlock);
		constexpr uint32 MIN_SPLIT_SIZE = 0x18; // smallest gap that is turned into a free block
		constexpr uint16 MAGIC_FREE = 0x4652; // 'FR'
		constexpr uint16 MAGIC_USED = 0x5544; // 'UD'
		constexpr uint32 ATTR_FROM_TAIL = 0x80000000;
		constexpr uint32 GAP_MASK = 0x7FFFFF;

		// a byte range of guest memory [begin, end)
		struct Range
		{
			uint32 begin;
			uint32 end;

			uint32 Length() const { return end - begin; }
		};

		uint32 AddrOf(const void* p)
		{
			return MEMPTR<const void>(p).GetMPTR();
		}

		uint8* HostOf(uint32 addr)
		{
			return (uint8*)MEMPTR<uint8>(addr).GetPtr();
		}

		MEMExpHeapBlock* BlockAt(uint32 addr)
		{
			return (MEMExpHeapBlock*)HostOf(addr);
		}

		MEMExpHeapBlock* HeaderOfData(const void* data)
		{
			return BlockAt(AddrOf(data) - HEADER_SIZE);
		}

		uint32 DataOf(MEMExpHeapBlock* block)
		{
			return AddrOf(block) + HEADER_SIZE;
		}

		uint32 EndOf(MEMExpHeapBlock* block)
		{
			return DataOf(block) + block->size;
		}

		// full range owned by a block, including the gap in front of its header
		Range RangeOf(MEMExpHeapBlock* block)
		{
			const uint32 gap = ((uint32)block->attribute >> 8) & GAP_MASK;
			return { AddrOf(block) - gap, EndOf(block) };
		}

		// turn a range into a block with a fresh header at its start
		MEMExpHeapBlock* MakeBlock(Range r, uint16 magic)
		{
			MEMExpHeapBlock* block = BlockAt(r.begin);
			memset(block, 0, HEADER_SIZE);
			block->magic = magic;
			block->size = r.end - (r.begin + HEADER_SIZE);
			return block;
		}

		// unlinks a block and returns the block that was in front of it
		MEMExpHeapBlock* Unlink(MEMExpHeapBlockList& list, MEMExpHeapBlock* block)
		{
			MEMExpHeapBlock* before = block->prev.GetPtr();
			MEMExpHeapBlock* after = block->next.GetPtr();
			if (before)
				before->next = after;
			else
				list.head = after;
			if (after)
				after->prev = before;
			else
				list.tail = before;
			return before;
		}

		// links a block behind 'before' (at the head when 'before' is null) and returns it
		MEMExpHeapBlock* LinkAfter(MEMExpHeapBlockList& list, MEMExpHeapBlock* block, MEMExpHeapBlock* before)
		{
			MEMExpHeapBlock* after;
			if (before)
			{
				after = before->next.GetPtr();
				before->next = block;
			}
			else
			{
				after = list.head.GetPtr();
				list.head = block;
			}
			block->prev = before;
			block->next = after;
			if (after)
				after->prev = block;
			else
				list.tail = block;
			return block;
		}

		MEMExpHeap* AsExpHeap(MEMHeapHandle heap)
		{
			cemu_assert_debug(heap != MEM_HEAP_INVALID_HANDLE);
			cemu_assert_debug(heap->magic == MEMHeapMagic::EXP_HEAP);
			return (MEMExpHeap*)heap;
		}

		// memset only uses the low byte of the fill value
		void FillRange(uint32 begin, uint32 length, uint32 value)
		{
			memset(HostOf(begin), (int)value, length);
		}

		// alignment helpers; the mask is the sign-extended complement of (alignment - 1)
		uint64 AlignMask(sint32 alignMinusOne)
		{
			return ~(uint64)(sint64)alignMinusOne;
		}

		uint64 AlignUp(uint64 value, sint32 alignMinusOne)
		{
			return (value + (uint64)(sint64)alignMinusOne) & AlignMask(alignMinusOne);
		}

		sint32 NegateAlignment(sint32 alignment)
		{
			return (sint32)(0u - (uint32)alignment);
		}

		// Gives a range back to the free list, merging it with free neighbours that touch it.
		// Returns false (and changes nothing) when the result would be too small for a header.
		bool ReturnRange(MEMExpHeap* heap, Range r)
		{
			MEMExpHeapBlockList& freeList = heap->freeBlocks;
			const bool fill = heap->HasOptionFill();
			Range merged = r;
			MEMExpHeapBlock* before = nullptr;
			for (MEMExpHeapBlock* it = freeList.head.GetPtr(); it; it = it->next.GetPtr())
			{
				const uint32 addr = AddrOf(it);
				if (addr < r.begin)
				{
					before = it;
					continue;
				}
				if (addr == r.end)
				{
					// the free block right behind joins in
					merged.end = EndOf(it);
					Unlink(freeList, it);
					if (fill)
						FillRange(addr, HEADER_SIZE, MEMGetFillValForHeap(HEAP_FILL_TYPE::ON_FREE));
				}
				break;
			}
			if (before && EndOf(before) == r.begin)
			{
				// so does the free block right in front
				merged.begin = AddrOf(before);
				before = Unlink(freeList, before);
			}
			if (merged.Length() < HEADER_SIZE)
				return false;
			if (fill)
				FillRange(r.begin, r.Length(), MEMGetFillValForHeap(HEAP_FILL_TYPE::ON_FREE));
			LinkAfter(freeList, MakeBlock(merged, MAGIC_FREE), before);
			return true;
		}

		// turns part of a free block into a used block whose data starts at dataAddr
		void* TakeFromFreeBlock(MEMExpHeap* heap, MEMExpHeapBlock* freeBlock, uint32 dataAddr, uint32 size, MEMExpHeapAllocDirection dir)
		{
			const bool keepMargins = ((uint16)heap->attribute & MEM_EXPHEAP_USE_ALIGN_MARGIN) != 0;
			const Range whole = RangeOf(freeBlock);
			Range front = { whole.begin, dataAddr - HEADER_SIZE };
			Range back = { dataAddr + size, whole.end };

			MEMExpHeapBlock* before = Unlink(heap->freeBlocks, freeBlock);
			if (front.Length() >= MIN_SPLIT_SIZE && (dir == MEMExpHeapAllocDirection::TAIL || keepMargins))
				before = LinkAfter(heap->freeBlocks, MakeBlock(front, MAGIC_FREE), before);
			else
				front.end = front.begin; // the gap belongs to the new block
			if (back.Length() >= MIN_SPLIT_SIZE && (dir == MEMExpHeapAllocDirection::HEAD || keepMargins))
				LinkAfter(heap->freeBlocks, MakeBlock(back, MAGIC_FREE), before);
			else
				back.begin = back.end;

			if (heap->HasOptionClear())
				FillRange(front.end, back.begin - front.end, 0);
			else if (heap->HasOptionFill())
				FillRange(front.end, back.begin - front.end, MEMGetFillValForHeap(HEAP_FILL_TYPE::ON_ALLOC));

			MEMExpHeapBlock* used = MakeBlock({ dataAddr - HEADER_SIZE, back.begin }, MAGIC_USED);
			uint32 attr = (dir == MEMExpHeapAllocDirection::TAIL) ? ATTR_FROM_TAIL : 0;
			attr |= (((dataAddr - HEADER_SIZE) - front.end) & GAP_MASK) << 8;
			attr |= (uint16)heap->groupId & 0xFF;
			used->attribute = attr;
			LinkAfter(heap->usedBlocks, used, heap->usedBlocks.tail.GetPtr());
			return HostOf(dataAddr);
		}

		bool UsesFirstFit(MEMExpHeap* heap)
		{
			return ((uint16)heap->attribute & 1) == MEM_EXPHEAP_ALLOC_MODE_FIRST;
		}

		void* AllocLow(MEMExpHeap* heap, uint32 size, sint32 alignment)
		{
			const sint32 alignMinusOne = (sint32)((uint32)alignment - 1);
			const bool firstFit = UsesFirstFit(heap);
			MEMExpHeapBlock* chosen = nullptr;
			uint32 chosenData = 0;
			uint32 chosenSize = 0xFFFFFFFF;
			for (MEMExpHeapBlock* it = heap->freeBlocks.head.GetPtr(); it; it = it->next.GetPtr())
			{
				const uint64 data = DataOf(it);
				const uint64 aligned = AlignUp(data, alignMinusOne);
				const uint32 blockSize = it->size;
				if ((uint64)blockSize < (aligned - data) + size)
					continue;
				if (blockSize >= chosenSize)
					continue;
				chosen = it;
				chosenData = (uint32)aligned;
				chosenSize = blockSize;
				if (firstFit || blockSize == size)
					break;
			}
			if (!chosen)
				return nullptr;
			return TakeFromFreeBlock(heap, chosen, chosenData, size, MEMExpHeapAllocDirection::HEAD);
		}

		void* AllocHigh(MEMExpHeap* heap, uint32 size, sint32 alignment)
		{
			const sint32 alignMinusOne = (sint32)((uint32)alignment - 1);
			const bool firstFit = UsesFirstFit(heap);
			MEMExpHeapBlock* chosen = nullptr;
			uint32 chosenData = 0;
			uint32 chosenSize = 0xFFFFFFFF;
			for (MEMExpHeapBlock* it = heap->freeBlocks.tail.GetPtr(); it; it = it->prev.GetPtr())
			{
				const uint64 data = DataOf(it);
				const uint32 blockSize = it->size;
				if (data + blockSize < size)
					continue;
				const uint64 aligned = (data + blockSize - size) & AlignMask(alignMinusOne);
				if (aligned < data)
					continue;
				if (blockSize >= chosenSize)
					continue;
				chosen = it;
				chosenData = (uint32)aligned;
				chosenSize = blockSize;
				if (firstFit || blockSize == size)
					break;
			}
			if (!chosen)
				return nullptr;
			return TakeFromFreeBlock(heap, chosen, chosenData, size, MEMExpHeapAllocDirection::TAIL);
		}
	}

	/* Exported functions */

	MEMHeapHandle MEMCreateExpHeapEx(void* startAddress, uint32 size, uint32 createFlags)
	{
		if (!startAddress)
			return MEM_HEAP_INVALID_HANDLE;
		const uint64 begin = ((uint64)AddrOf(startAddress) + 3) & ~3ull;
		const uint64 end = ((uint64)AddrOf(startAddress) + size) & ~3ull;
		if (begin > end || end - begin < 0x6C)
			return MEM_HEAP_INVALID_HANDLE;

		MEMExpHeap* heap = (MEMExpHeap*)HostOf((uint32)begin);
		MEMInitHeapBase(heap, MEMHeapMagic::EXP_HEAP, HostOf((uint32)begin + sizeof(MEMExpHeap)), HostOf((uint32)end), createFlags);
		MEMExpHeapBlock* all = MakeBlock({ heap->heapStart.GetMPTR(), heap->heapEnd.GetMPTR() }, MAGIC_FREE);
		heap->freeBlocks.head = all;
		heap->freeBlocks.tail = all;
		heap->usedBlocks.head = nullptr;
		heap->usedBlocks.tail = nullptr;
		heap->groupId = 0;
		heap->attribute = 0;
		return heap;
	}

	void* MEMDestroyExpHeap(MEMHeapHandle heap)
	{
		AsExpHeap(heap);
		MEMBaseDestroyHeap(heap);
		return heap;
	}

	void* MEMAllocFromExpHeapEx(MEMHeapHandle heap, uint32 size, sint32 alignment)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		if (alignment == 0)
		{
			// would crash on the console
			cemuLog_log(LogType::Force, "MEMAllocFromExpHeapEx(): alignment 0 is not allowed");
			return nullptr;
		}
		if (size == 0)
			size = 1;
		size = (size + 3) & ~3u;
		expHeap->AcquireLock();
		void* mem;
		if (alignment > 0)
			mem = AllocLow(expHeap, size, alignment);
		else
			mem = AllocHigh(expHeap, size, NegateAlignment(alignment));
		expHeap->ReleaseLock();
		return mem;
	}

	void MEMFreeToExpHeap(MEMHeapHandle heap, void* mem)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		if (!mem)
			return;
		expHeap->AcquireLock();
		MEMExpHeapBlock* block = HeaderOfData(mem);
		const Range r = RangeOf(block);
		Unlink(expHeap->usedBlocks, block);
		ReturnRange(expHeap, r);
		expHeap->ReleaseLock();
	}

	// note: the mode bit can be set but not cleared again (kept for compatibility)
	uint16 MEMSetAllocModeForExpHeap(MEMHeapHandle heap, uint16 mode)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		expHeap->AcquireLock();
		const uint16 previous = (uint16)expHeap->attribute & 1;
		expHeap->attribute = (uint16)((uint16)expHeap->attribute | (mode & 1));
		expHeap->ReleaseLock();
		return previous;
	}

	uint16 MEMGetAllocModeForExpHeap(MEMHeapHandle heap)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		expHeap->AcquireLock();
		const uint16 mode = (uint16)expHeap->attribute & 1;
		expHeap->ReleaseLock();
		return mode;
	}

	// gives a free block at the end of the heap back to the parent and returns the new heap size
	uint32 MEMAdjustExpHeap(MEMHeapHandle heap)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		expHeap->AcquireLock();
		uint32 newSize = 0;
		MEMExpHeapBlock* last = expHeap->freeBlocks.tail.GetPtr();
		if (last && EndOf(last) == expHeap->heapEnd.GetMPTR())
		{
			Unlink(expHeap->freeBlocks, last);
			const uint32 newEnd = expHeap->heapEnd.GetMPTR() - (HEADER_SIZE + last->size);
			expHeap->heapEnd = HostOf(newEnd);
			newSize = newEnd - AddrOf(expHeap);
		}
		expHeap->ReleaseLock();
		return newSize;
	}

	// grows or shrinks a used block in place; returns the new size or 0 when it cannot grow
	uint32 MEMResizeForMBlockExpHeap(MEMHeapHandle heap, void* memBlock, uint32 size)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		expHeap->AcquireLock();
		MEMExpHeapBlock* block = HeaderOfData(memBlock);
		const uint32 data = AddrOf(memBlock);
		const uint32 oldSize = block->size;
		uint32 result = oldSize;
		if (size < oldSize)
		{
			block->size = size;
			if (!ReturnRange(expHeap, { data + size, data + oldSize }))
				block->size = oldSize; // the cut-off part is too small to stand alone
			result = block->size;
		}
		else if (size > oldSize)
		{
			result = 0;
			MEMExpHeapBlock* neighbour = nullptr;
			for (MEMExpHeapBlock* it = expHeap->freeBlocks.head.GetPtr(); it; it = it->next.GetPtr())
			{
				if (AddrOf(it) == data + oldSize)
				{
					neighbour = it;
					break;
				}
			}
			if (neighbour && (uint64)size <= (uint64)(uint32)(oldSize + neighbour->size) + HEADER_SIZE)
			{
				Range rest = RangeOf(neighbour);
				MEMExpHeapBlock* before = Unlink(expHeap->freeBlocks, neighbour);
				const uint32 grownFrom = rest.begin;
				rest.begin = data + size;
				if (rest.Length() < HEADER_SIZE)
					rest.begin = rest.end; // swallow the whole neighbour
				block->size = rest.begin - data;
				if (rest.Length() >= HEADER_SIZE)
					LinkAfter(expHeap->freeBlocks, MakeBlock(rest, MAGIC_FREE), before);
				if (expHeap->HasOptionClear())
					FillRange(grownFrom, rest.begin - grownFrom, 0);
				else if (expHeap->HasOptionFill())
					FillRange(grownFrom, rest.begin - grownFrom, MEMGetFillValForHeap(HEAP_FILL_TYPE::ON_ALLOC));
				result = block->size;
			}
		}
		expHeap->ReleaseLock();
		return result;
	}

	uint32 MEMGetTotalFreeSizeForExpHeap(MEMHeapHandle heap)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		expHeap->AcquireLock();
		uint32 total = 0;
		for (MEMExpHeapBlock* it = expHeap->freeBlocks.head.GetPtr(); it; it = it->next.GetPtr())
			total += it->size;
		expHeap->ReleaseLock();
		return total;
	}

	// largest single allocation possible with the given alignment
	uint32 MEMGetAllocatableSizeForExpHeapEx(MEMHeapHandle heap, sint32 alignment)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		if (alignment < 0)
			alignment = NegateAlignment(alignment);
		const sint32 alignMinusOne = (sint32)((uint32)alignment - 1);
		expHeap->AcquireLock();
		uint32 best = 0;
		uint32 bestGap = 0xFFFFFFFF;
		for (MEMExpHeapBlock* it = expHeap->freeBlocks.head.GetPtr(); it; it = it->next.GetPtr())
		{
			const uint64 data = DataOf(it);
			const uint64 end = data + it->size;
			const uint64 aligned = AlignUp(data, alignMinusOne);
			if (aligned >= end)
				continue;
			const uint32 usable = (uint32)(end - aligned);
			const uint32 gap = (uint32)(aligned - data);
			// prefer the larger size, then the smaller alignment gap
			if (usable < best || (usable == best && bestGap <= gap))
				continue;
			best = usable;
			bestGap = gap;
		}
		expHeap->ReleaseLock();
		return best;
	}

	uint16 MEMSetGroupIDForExpHeap(MEMHeapHandle heap, uint16 groupId)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		expHeap->AcquireLock();
		const uint16 previous = expHeap->groupId;
		expHeap->groupId = groupId;
		expHeap->ReleaseLock();
		return previous;
	}

	uint16 MEMGetGroupIDForExpHeap(MEMHeapHandle heap)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		expHeap->AcquireLock();
		const uint16 groupId = expHeap->groupId;
		expHeap->ReleaseLock();
		return groupId;
	}

	// calls visitor(mem, heap, userParam) for every used block, oldest first
	void MEMVisitAllocatedForExpHeap(MEMHeapHandle heap, MEMPTR<void> visitor, uint32 userParam)
	{
		MEMExpHeap* expHeap = AsExpHeap(heap);
		expHeap->AcquireLock();
		MEMExpHeapBlock* it = expHeap->usedBlocks.head.GetPtr();
		while (it)
		{
			PPCCoreCallback(visitor.GetMPTR(), DataOf(it), AddrOf(expHeap), userParam);
			it = it->next.GetPtr();
		}
		expHeap->ReleaseLock();
	}

	uint32 MEMGetSizeForMBlockExpHeap(void* memBlock)
	{
		return HeaderOfData(memBlock)->size;
	}

	uint16 MEMGetGroupIDForMBlockExpHeap(void* memBlock)
	{
		return (uint32)HeaderOfData(memBlock)->attribute & 0xFF;
	}

	uint16 MEMGetAllocDirForMBlockExpHeap(void* memBlock)
	{
		return ((uint32)HeaderOfData(memBlock)->attribute >> 31) & 1;
	}

	uint32 MEMCheckExpHeap(MEMHeapHandle heap, uint32 options)
	{
		if (heap == MEM_HEAP_INVALID_HANDLE || heap->magic != MEMHeapMagic::EXP_HEAP)
			return 0;
		heap->AcquireLock();
		heap->ReleaseLock();
		return 1;
	}

	uint32 MEMCheckForMBlockExpHeap(void* memBlock, MEMHeapHandle heap, uint32 options)
	{
		return 1;
	}

	/* MEMAllocator for expanded heaps */

	// alloc(allocator, size) / free(allocator, ptr)
	static void ExpHeapAllocatorAlloc(PPCInterpreter_t* hCPU)
	{
		MEMAllocator* allocator = MEMPTR<MEMAllocator>(hCPU->gpr[3]).GetPtr();
		void* mem = MEMAllocFromExpHeapEx((MEMHeapHandle)allocator->heap.GetPtr(), hCPU->gpr[4], (sint32)(uint32)allocator->param1);
		osLib_returnFromFunction(hCPU, MEMPTR<void>(mem).GetMPTR());
	}

	static void ExpHeapAllocatorFree(PPCInterpreter_t* hCPU)
	{
		MEMAllocator* allocator = MEMPTR<MEMAllocator>(hCPU->gpr[3]).GetPtr();
		MEMFreeToExpHeap((MEMHeapHandle)allocator->heap.GetPtr(), MEMPTR<void>(hCPU->gpr[4]).GetPtr());
		osLib_returnFromFunction(hCPU, 0);
	}

	SysAllocator<MEMAllocatorFunc> g_expHeapAllocatorFuncs;

	void MEMInitAllocatorForExpHeap(MEMAllocator* allocator, MEMHeapHandle heap, sint32 alignment)
	{
		allocator->func = g_expHeapAllocatorFuncs.GetPtr();
		g_expHeapAllocatorFuncs->funcAlloc = PPCInterpreter_makeCallableExportDepr(ExpHeapAllocatorAlloc);
		g_expHeapAllocatorFuncs->funcFree = PPCInterpreter_makeCallableExportDepr(ExpHeapAllocatorFree);
		allocator->heap = heap;
		allocator->param1 = alignment;
		allocator->param2 = 0;
	}

	void expheap_load()
	{
		cafeExportRegister("coreinit", MEMCreateExpHeapEx, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMDestroyExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMAllocFromExpHeapEx, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMFreeToExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMSetAllocModeForExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetAllocModeForExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMAdjustExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMResizeForMBlockExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetTotalFreeSizeForExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetAllocatableSizeForExpHeapEx, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMSetGroupIDForExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetGroupIDForExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMVisitAllocatedForExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetSizeForMBlockExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetGroupIDForMBlockExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetAllocDirForMBlockExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMCheckExpHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMCheckForMBlockExpHeap, LogType::CoreinitMem);

		cafeExportRegister("coreinit", MEMInitAllocatorForExpHeap, LogType::CoreinitMem);
	}
}
