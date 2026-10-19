// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/OS/libs/coreinit/coreinit_Spinlock.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"

namespace coreinit
{
	// registers the MEM*ExpHeap* exports
	void expheap_load();

	// heap attribute bits (MEMExpHeap::attribute)
#define MEM_EXPHEAP_ALLOC_MODE_FIRST	(0)	// bit 0 clear: take the first block that fits
#define MEM_EXPHEAP_ALLOC_MODE_NEAR		(1)	// bit 0 set: take the smallest block that fits
#define MEM_EXPHEAP_USE_ALIGN_MARGIN	(2)	// keep alignment gaps as free blocks

	enum class MEMExpHeapAllocDirection : uint32
	{
		HEAD = 0,
		TAIL = 1
	};

	struct MEMExpHeapBlockList
	{
		MEMPTR<struct MEMExpHeapBlock> head;
		MEMPTR<struct MEMExpHeapBlock> tail;
	};
	static_assert(sizeof(MEMExpHeapBlockList) == 8);

	// expanded heap: blocks with a 0x14 byte header, kept in an address-ordered free list
	// and an allocation-ordered used list
	struct MEMExpHeap : MEMHeapBase
	{
		/* +0x34 */ uint32be _pad34;
		/* +0x38 */ uint32be _pad38;
		/* +0x3C */ uint32be _pad3C;
		/* +0x40 */ MEMExpHeapBlockList freeBlocks;
		/* +0x48 */ MEMExpHeapBlockList usedBlocks;
		/* +0x50 */ uint16be groupId;
		/* +0x52 */ uint16be attribute;
	};
	static_assert(offsetof(MEMExpHeap, freeBlocks) == 0x40);
	static_assert(offsetof(MEMExpHeap, groupId) == 0x50);
	static_assert(sizeof(MEMExpHeap) == 0x54);

	MEMHeapHandle MEMCreateExpHeapEx(void* startAddress, uint32 size, uint32 createFlags);
	void* MEMAllocFromExpHeapEx(MEMHeapHandle heap, uint32 size, sint32 alignment);
	void MEMFreeToExpHeap(MEMHeapHandle heap, void* mem);
	uint32 MEMGetAllocatableSizeForExpHeapEx(MEMHeapHandle heap, sint32 alignment);
}
