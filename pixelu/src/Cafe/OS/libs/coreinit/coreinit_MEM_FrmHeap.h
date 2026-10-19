// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"

namespace coreinit
{
	// snapshot taken by MEMRecordStateForFrmHeap, stored inside the heap itself
	struct MEMFrmHeapRecordedState
	{
		uint32be id;
		MEMPTR<void> allocationHead;
		MEMPTR<void> allocationTail;
		MEMPTR<MEMFrmHeapRecordedState> prevRecordedState;
	};
	static_assert(sizeof(MEMFrmHeapRecordedState) == 0x10);

	// frame heap: allocations grow from the start (head) and from the end (tail) towards each other
	struct MEMFrmHeap : MEMHeapBase
	{
		/* +0x34 */ uint32be _pad34;
		/* +0x38 */ uint32be _pad38;
		/* +0x3C */ uint32be _pad3C;
		/* +0x40 */ MEMPTR<void> allocationHead; // first free byte
		/* +0x44 */ MEMPTR<void> allocationTail; // one past the last free byte
		/* +0x48 */ MEMPTR<MEMFrmHeapRecordedState> recordedStates;
	};
	static_assert(offsetof(MEMFrmHeap, allocationHead) == 0x40);
	static_assert(sizeof(MEMFrmHeap) == 0x4C);

	enum class FrmHeapMode : uint32
	{
		Head = (1 << 0),
		Tail = (1 << 1),
		All = (Head | Tail),
	};

	MEMFrmHeap* MEMCreateFrmHeapEx(void* memStart, uint32 size, uint32 createFlags);
	void* MEMDestroyFrmHeap(MEMFrmHeap* frmHeap);

	void* MEMAllocFromFrmHeapEx(MEMFrmHeap* frmHeap, uint32 size, sint32 alignment);
	void MEMFreeToFrmHeap(MEMFrmHeap* frmHeap, FrmHeapMode mode);

	void InitializeMEMFrmHeap();
}
ENABLE_BITMASK_OPERATORS(coreinit::FrmHeapMode);
