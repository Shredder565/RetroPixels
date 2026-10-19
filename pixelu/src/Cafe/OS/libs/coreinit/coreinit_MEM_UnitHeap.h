// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"

namespace coreinit
{
	// a free unit holds the address of the next free unit in its first word
	struct MEMUnitHeapBlock
	{
		MEMPTR<MEMUnitHeapBlock> nextBlock;
	};
	static_assert(sizeof(MEMUnitHeapBlock) == 4);

	// unit heap: fixed-size units handed out from a singly linked free list
	struct MEMUnitHeap : public MEMHeapBase
	{
		/* +0x34 */ uint32 _pad34;
		/* +0x38 */ uint32 _pad38;
		/* +0x3C */ uint32 _pad3C;
		/* +0x40 */ MEMPTR<MEMUnitHeapBlock> firstFreeBlock;
		/* +0x44 */ uint32be blockSize;
	};
	static_assert(offsetof(MEMUnitHeap, firstFreeBlock) == 0x40);
	static_assert(sizeof(MEMUnitHeap) == 0x48);

	MEMHeapBase* MEMCreateUnitHeapEx(void* memStart, uint32 heapSize, uint32 memBlockSize, uint32 alignment, uint32 createFlags);
	void* MEMDestroyUnitHeap(MEMHeapHandle hHeap);
}
