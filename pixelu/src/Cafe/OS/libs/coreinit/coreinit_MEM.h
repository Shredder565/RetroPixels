// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/OS/libs/coreinit/coreinit_Spinlock.h"

// MEMAllocator: a guest object holding an alloc/free function pair plus the heap they act on.
// MEMAllocFromAllocator(allocator, size) and MEMFreeToAllocator(allocator, ptr) call through it.
struct MEMAllocatorFunc
{
	MEMPTR<void> funcAlloc; // (MEMAllocator*, size) -> ptr
	MEMPTR<void> funcFree;  // (MEMAllocator*, ptr)
};
static_assert(sizeof(MEMAllocatorFunc) == 8);

struct MEMAllocator
{
	/* +0x00 */ MEMPTR<MEMAllocatorFunc> func;
	/* +0x04 */ MEMPTR<void> heap;
	/* +0x08 */ uint32be param1; // alignment for heap allocators
	/* +0x0C */ uint32be param2;
};
static_assert(sizeof(MEMAllocator) == 0x10);

// Bump allocator for emulator-owned objects that live in guest memory (the "Cemu area" range).
// Memory handed out here is never returned.
MPTR coreinit_allocFromSysArea(uint32 size, uint32 alignment);
void coreinit_freeToSysArea(MPTR mem);

void coreinitExport_MEMInitAllocatorForDefaultHeap(PPCInterpreter_t* hCPU);

namespace coreinit
{
#define MEM_HEAP_INVALID_HANDLE (nullptr)
#define MEM_HEAP_DEFAULT_ALIGNMENT 4
#define MIN_ALIGNMENT 4
#define MIN_ALIGNMENT_MINUS_ONE (MIN_ALIGNMENT-1)

	// heap creation flags (stored in MEMHeapBase::flags)
#define MEM_HEAP_OPTION_NONE		(0)
#define MEM_HEAP_OPTION_CLEAR		(1 << 0)
#define MEM_HEAP_OPTION_FILL		(1 << 1)
#define MEM_HEAP_OPTION_THREADSAFE	(1 << 2)

	enum class MEMHeapMagic : uint32
	{
		UNIT_HEAP = 'UNTH',
		BLOCK_HEAP = 'BLKH',
		FRAME_HEAP = 'FRMH',
		EXP_HEAP = 'EXPH',
		USER_HEAP = 'USRH',
	};

	// intrusive doubly linked list; 'offset' is where the MEMLink sits inside each object
	struct MEMLink
	{
		MEMPTR<void> prev;
		MEMPTR<void> next;
	};
	static_assert(sizeof(MEMLink) == 0x8);

	struct MEMList
	{
		/* 0x00 */ MEMPTR<void> head;
		/* 0x04 */ MEMPTR<void> tail;
		/* 0x08 */ uint16be numObjects;
		/* 0x0A */ uint16be offset;
	};
	static_assert(sizeof(MEMList) == 0xC);

	void MEMInitList(MEMList* list, uint32 offset);
	void MEMAppendListObject(MEMList* list, void* object);
	void MEMRemoveListObject(MEMList* list, void* object);

	void* MEMGetFirstListObject(MEMList* list);
	void* MEMGetNextListObject(MEMList* list, void* object);

	// common header at the start of every heap
	struct MEMHeapBase
	{
		/* +0x00 */ betype<MEMHeapMagic> magic;
		/* +0x04 */ MEMLink link;        // entry in the parent's child list
		/* +0x0C */ MEMList childList;   // heaps created inside this heap
		/* +0x18 */ MEMPTR<void> heapStart;
		/* +0x1C */ MEMPTR<void> heapEnd; // one past the last byte
		/* +0x20 */ OSSpinLock spinlock;
		/* +0x30 */ uint8 _ukn[3];
		/* +0x33 */ uint8 flags;          // MEM_HEAP_OPTION_*

		void AcquireLock()
		{
			if (flags & MEM_HEAP_OPTION_THREADSAFE)
				OSUninterruptibleSpinLock_Acquire(&spinlock);
		}

		void ReleaseLock()
		{
			if (flags & MEM_HEAP_OPTION_THREADSAFE)
				OSUninterruptibleSpinLock_Release(&spinlock);
		}

		bool HasOptionClear() const
		{
			return (flags & MEM_HEAP_OPTION_CLEAR) != 0;
		}

		bool HasOptionFill() const
		{
			return (flags & MEM_HEAP_OPTION_FILL) != 0;
		}
	};
	static_assert(offsetof(MEMHeapBase, childList) == 0xC);
	static_assert(offsetof(MEMHeapBase, spinlock) == 0x20);
	static_assert(offsetof(MEMHeapBase, flags) == 0x33);
	static_assert(sizeof(MEMHeapBase) == 0x34);

	typedef MEMHeapBase* MEMHeapHandle;

	// heap base
	void MEMInitHeapBase(MEMHeapBase* heap, MEMHeapMagic magic, void* heapStart, void* heapEnd, uint32 createFlags);
	void MEMBaseDestroyHeap(MEMHeapBase* heap);

	MEMHeapBase* MEMGetBaseHeapHandle(uint32 index);
	MEMHeapBase* MEMSetBaseHeapHandle(uint32 index, MEMHeapBase* heapBase);

	// fill values used by heaps created with MEM_HEAP_OPTION_FILL
	enum class HEAP_FILL_TYPE : uint32
	{
		ON_HEAP_CREATE = 0,
		ON_ALLOC = 1,
		ON_FREE = 2,
	};

	uint32 MEMGetFillValForHeap(HEAP_FILL_TYPE type);
	uint32 MEMSetFillValForHeap(HEAP_FILL_TYPE type, uint32 value);
	MEMHeapHandle MEMFindContainHeap(const void* memBlock);

	// default heap (MEM2 expanded heap) and the guest-callable allocator entry points
	void InitDefaultHeaps(MEMPTR<MEMHeapBase>& mem1Heap, MEMPTR<MEMHeapBase>& memFGHeap, MEMPTR<MEMHeapBase>& mem2Heap);

	void* default_MEMAllocFromDefaultHeap(uint32 size);
	void* default_MEMAllocFromDefaultHeapEx(uint32 size, sint32 alignment);
	void default_MEMFreeToDefaultHeap(void* mem);

	// these go through the guest-visible function pointers, so a game that replaces
	// the default heap functions gets these calls too
	void* _weak_MEMAllocFromDefaultHeapEx(uint32 size, sint32 alignment);
	void* _weak_MEMAllocFromDefaultHeap(uint32 size);
	void _weak_MEMFreeToDefaultHeap(void* ptr);

	void InitializeMEMUnitHeap();

	void InitializeMEM();
}
