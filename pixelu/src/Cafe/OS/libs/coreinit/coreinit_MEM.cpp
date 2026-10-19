// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_Memory.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_FrmHeap.h"
#include "coreinit_DynLoad.h"

/* Emulator system area
 * A simple bump allocator over the guest range reserved for the emulator. Objects placed here
 * (SysAllocator globals, HLE helper structs) stay for the lifetime of the process, so the
 * cursor is never rewound. Every allocation is rounded up to a multiple of 4 bytes. */

namespace
{
	std::mutex g_sysAreaLock;
	uint32 g_sysAreaCursor = 0; // offset from the start of the area
}

MPTR coreinit_allocFromSysArea(uint32 size, uint32 alignment)
{
	cemu_assert_debug(mmuRange_CEMU_AREA.isMapped());
	std::lock_guard _lock(g_sysAreaLock);
	// alignment does not have to be a power of two here
	uint32 start = g_sysAreaCursor + alignment - 1;
	start -= start % alignment;
	g_sysAreaCursor = start + ((size + 3) & ~3u);
	if (g_sysAreaCursor >= mmuRange_CEMU_AREA.getSize())
	{
		cemuLog_log(LogType::Force, "Ran out of system memory");
		cemu_assert(false);
	}
	return mmuRange_CEMU_AREA.getBase() + start;
}

void coreinit_freeToSysArea(MPTR mem)
{
	// system area memory is never reused
}

namespace coreinit
{
	/* Intrusive object lists (MEMList) */

	static MEMLink* LinkOf(MEMList* list, void* object)
	{
		return (MEMLink*)((uint8*)object + (uint16)list->offset);
	}

	void MEMInitList(MEMList* list, uint32 offset)
	{
		cemu_assert(offset <= 0xFFFF);
		list->head = nullptr;
		list->tail = nullptr;
		list->numObjects = 0;
		list->offset = (uint16)offset;
	}

	void MEMAppendListObject(MEMList* list, void* object)
	{
		MEMLink* link = LinkOf(list, object);
		link->next = nullptr;
		link->prev = list->tail;
		if (list->tail)
			LinkOf(list, list->tail.GetPtr())->next = object;
		else
			list->head = object;
		list->tail = object;
		list->numObjects = list->numObjects + 1;
	}

	void MEMPrependListObject(MEMList* list, void* object)
	{
		MEMLink* link = LinkOf(list, object);
		link->prev = nullptr;
		link->next = list->head;
		if (list->head)
			LinkOf(list, list->head.GetPtr())->prev = object;
		else
			list->tail = object;
		list->head = object;
		list->numObjects = list->numObjects + 1;
	}

	void MEMRemoveListObject(MEMList* list, void* object)
	{
		MEMLink* link = LinkOf(list, object);
		MEMPTR<void> before = link->prev;
		MEMPTR<void> after = link->next;
		if (before)
			LinkOf(list, before.GetPtr())->next = after;
		else
			list->head = after;
		if (after)
			LinkOf(list, after.GetPtr())->prev = before;
		else
			list->tail = before;
		link->prev = nullptr;
		link->next = nullptr;
		list->numObjects = list->numObjects - 1;
	}

	void* MEMGetNextListObject(MEMList* list, void* object)
	{
		if (object == nullptr)
			return list->head.GetPtr();
		return LinkOf(list, object)->next.GetPtr();
	}

	void* MEMGetPrevListObject(MEMList* list, void* object)
	{
		if (object == nullptr)
			return list->tail.GetPtr();
		return LinkOf(list, object)->prev.GetPtr();
	}

	void* MEMGetFirstListObject(MEMList* list)
	{
		return list->head.GetPtr();
	}

	void* MEMGetNthListObject(MEMList* list, uint32 index)
	{
		void* object = list->head.GetPtr();
		while (object && index != 0)
		{
			object = LinkOf(list, object)->next.GetPtr();
			index--;
		}
		return object;
	}

	/* Heap tree
	 * Every heap is registered in the child list of the heap that contains its header, or in one
	 * of three root lists: MEM1, the foreground bucket, or everything else (MEM2). */

	enum class HeapArena
	{
		Other,
		Mem1,
		Foreground,
	};

	struct
	{
		bool rootsReady = false;
		bool globalLockReady = false;
		MEMList rootOther;
		MEMList rootMem1;
		MEMList rootForeground;
		std::array<MEMHeapBase*, 9> baseHeaps{};
		uint32 fillValues[3] = { 0xC3C3C3C3, 0xF3F3F3F3, 0xD3D3D3D3 }; // create, alloc, free
		MEMHeapBase* defaultHeap = nullptr;
	}g_mem;

	SysAllocator<OSSpinLock> g_heapTreeLock;

	static bool InRange(uint32 addr, uint32 base, uint32 size)
	{
		return addr >= base && (uint64)addr < (uint64)base + size;
	}

	static HeapArena ArenaOf(const void* ptr)
	{
		const uint32 addr = MEMPTR<const void>(ptr).GetMPTR();
		MEMPTR<void> base;
		uint32be size;
		OSGetMemBound(1, &base, &size);
		if (InRange(addr, base.GetMPTR(), size))
			return HeapArena::Mem1;
		OSGetForegroundBucket(&base, &size);
		if (InRange(addr, base.GetMPTR(), size))
			return HeapArena::Foreground;
		return HeapArena::Other;
	}

	static MEMList* RootList(HeapArena arena)
	{
		switch (arena)
		{
		case HeapArena::Mem1:
			return &g_mem.rootMem1;
		case HeapArena::Foreground:
			return &g_mem.rootForeground;
		default:
			return &g_mem.rootOther;
		}
	}

	// innermost heap in the tree below 'list' whose memory range holds 'addr'
	static MEMHeapBase* InnermostHeapAt(MEMList* list, uint32 addr)
	{
		MEMHeapBase* found = nullptr;
		while (list)
		{
			MEMHeapBase* match = nullptr;
			for (void* obj = MEMGetFirstListObject(list); obj; obj = MEMGetNextListObject(list, obj))
			{
				MEMHeapBase* heap = (MEMHeapBase*)obj;
				if (addr >= heap->heapStart.GetMPTR() && addr < heap->heapEnd.GetMPTR())
				{
					match = heap;
					break;
				}
			}
			if (!match)
				break;
			found = match;
			list = &match->childList;
		}
		return found;
	}

	// the list a heap whose header is at 'heap' belongs in
	static MEMList* OwnerListFor(MEMHeapBase* heap)
	{
		MEMList* root = RootList(ArenaOf(heap));
		MEMHeapBase* parent = InnermostHeapAt(root, MEMPTR<MEMHeapBase>(heap).GetMPTR());
		return parent ? &parent->childList : root;
	}

	void MEMInitHeapBase(MEMHeapBase* heap, MEMHeapMagic magic, void* heapStart, void* heapEnd, uint32 createFlags)
	{
		memset(heap, 0, sizeof(MEMHeapBase));
		heap->magic = magic;
		heap->heapStart = heapStart;
		heap->heapEnd = heapEnd;
		heap->flags = (uint8)createFlags;
		MEMInitList(&heap->childList, offsetof(MEMHeapBase, link));

		if (!g_mem.globalLockReady)
		{
			OSInitSpinLock(g_heapTreeLock.GetPtr());
			g_mem.globalLockReady = true;
		}
		if (!g_mem.rootsReady)
		{
			MEMInitList(&g_mem.rootOther, offsetof(MEMHeapBase, link));
			MEMInitList(&g_mem.rootMem1, offsetof(MEMHeapBase, link));
			MEMInitList(&g_mem.rootForeground, offsetof(MEMHeapBase, link));
			g_mem.rootsReady = true;
		}
		OSInitSpinLock(&heap->spinlock);

		OSUninterruptibleSpinLock_Acquire(g_heapTreeLock.GetPtr());
		MEMAppendListObject(OwnerListFor(heap), heap);
		OSUninterruptibleSpinLock_Release(g_heapTreeLock.GetPtr());
	}

	void MEMBaseDestroyHeap(MEMHeapBase* heap)
	{
		OSUninterruptibleSpinLock_Acquire(g_heapTreeLock.GetPtr());
		heap->AcquireLock();
		MEMRemoveListObject(OwnerListFor(heap), heap);
		heap->ReleaseLock();
		OSUninterruptibleSpinLock_Release(g_heapTreeLock.GetPtr());
	}

	MEMHeapBase* MEMFindContainHeap(const void* memBlock)
	{
		const HeapArena arena = ArenaOf(memBlock);
		OSUninterruptibleSpinLock_Acquire(g_heapTreeLock.GetPtr());
		MEMHeapBase* heap = nullptr;
		// MEM1 and the foreground bucket only count while the application owns the foreground
		if (arena == HeapArena::Other || OSGetForegroundBucket(nullptr, nullptr))
			heap = InnermostHeapAt(RootList(arena), MEMPTR<const void>(memBlock).GetMPTR());
		OSUninterruptibleSpinLock_Release(g_heapTreeLock.GetPtr());
		return heap;
	}

	MEMHeapBase* MEMGetBaseHeapHandle(uint32 index)
	{
		if (index >= g_mem.baseHeaps.size())
			return nullptr;
		return g_mem.baseHeaps[index];
	}

	// returns the heap already in the slot (and leaves it there), or nullptr on success
	MEMHeapBase* MEMSetBaseHeapHandle(uint32 index, MEMHeapBase* heapBase)
	{
		if (index >= g_mem.baseHeaps.size())
			return nullptr;
		if (g_mem.baseHeaps[index])
		{
			cemuLog_log(LogType::Force, "MEMSetBaseHeapHandle(): slot {} is already in use", index);
			return g_mem.baseHeaps[index];
		}
		g_mem.baseHeaps[index] = heapBase;
		return nullptr;
	}

	uint32 MEMSetFillValForHeap(HEAP_FILL_TYPE type, uint32 value)
	{
		const uint32 slot = (uint32)type;
		if (slot >= 3)
			return 0;
		OSUninterruptibleSpinLock_Acquire(g_heapTreeLock.GetPtr());
		const uint32 previous = g_mem.fillValues[slot];
		g_mem.fillValues[slot] = value;
		OSUninterruptibleSpinLock_Release(g_heapTreeLock.GetPtr());
		return previous;
	}

	uint32 MEMGetFillValForHeap(HEAP_FILL_TYPE type)
	{
		const uint32 slot = (uint32)type;
		if (slot >= 3)
			return 0;
		OSUninterruptibleSpinLock_Acquire(g_heapTreeLock.GetPtr());
		const uint32 value = g_mem.fillValues[slot];
		OSUninterruptibleSpinLock_Release(g_heapTreeLock.GetPtr());
		return value;
	}

	// wraps memory the application manages itself in a heap header so MEMFindContainHeap knows it
	void* MEMCreateUserHeapHandle(void* heapAddress, uint32 heapSize)
	{
		uint8* base = (uint8*)heapAddress;
		MEMInitHeapBase((MEMHeapBase*)heapAddress, MEMHeapMagic::USER_HEAP, base + 0x40, base + heapSize, 0);
		return heapAddress;
	}

	/* Default heap */

	void* default_MEMAllocFromDefaultHeap(uint32 size)
	{
		void* mem = MEMAllocFromExpHeapEx(g_mem.defaultHeap, size, 0x40);
		cemuLog_logDebug(LogType::CoreinitMem, "MEMAllocFromDefaultHeap(0x{:08x}) -> 0x{:08x}", size, MEMPTR<void>(mem).GetMPTR());
		return mem;
	}

	void* default_MEMAllocFromDefaultHeapEx(uint32 size, sint32 alignment)
	{
		void* mem = MEMAllocFromExpHeapEx(g_mem.defaultHeap, size, alignment);
		cemuLog_logDebug(LogType::CoreinitMem, "MEMAllocFromDefaultHeapEx(0x{:08x}, {}) -> 0x{:08x}", size, alignment, MEMPTR<void>(mem).GetMPTR());
		return mem;
	}

	void default_MEMFreeToDefaultHeap(void* mem)
	{
		MEMFreeToExpHeap(g_mem.defaultHeap, mem);
	}

	// guest-callable entry points stored in the MEMAllocFromDefaultHeap(Ex)/MEMFreeToDefaultHeap pointers
	static void GuestDefaultAlloc(PPCInterpreter_t* hCPU)
	{
		MEMPTR<void> mem = default_MEMAllocFromDefaultHeap(hCPU->gpr[3]);
		osLib_returnFromFunction(hCPU, mem.GetMPTR());
	}

	static void GuestDefaultAllocEx(PPCInterpreter_t* hCPU)
	{
		MEMPTR<void> mem = default_MEMAllocFromDefaultHeapEx(hCPU->gpr[3], (sint32)hCPU->gpr[4]);
		osLib_returnFromFunction(hCPU, mem.GetMPTR());
	}

	static void GuestDefaultFree(PPCInterpreter_t* hCPU)
	{
		default_MEMFreeToDefaultHeap(MEMPTR<void>(hCPU->gpr[3]).GetPtr());
		osLib_returnFromFunction(hCPU, 0);
	}

	// OSDynLoad allocator: (size, alignment, uint32be* out) -> error code
	static void GuestDynLoadAlloc(PPCInterpreter_t* hCPU)
	{
		uint32be* out = MEMPTR<uint32be>(hCPU->gpr[5]).GetPtr();
		*out = PPCCoreCallback(gCoreinitData->MEMAllocFromDefaultHeapEx.GetMPTR(), (sint32)hCPU->gpr[3], (sint32)hCPU->gpr[4]);
		osLib_returnFromFunction(hCPU, 0);
	}

	static void GuestDynLoadFree(PPCInterpreter_t* hCPU)
	{
		MEMPTR<void> mem(hCPU->gpr[3]);
		PPCCoreCallback(gCoreinitData->MEMFreeToDefaultHeap.GetMPTR(), mem);
		osLib_returnFromFunction(hCPU, 0);
	}

	void* _weak_MEMAllocFromDefaultHeapEx(uint32 size, sint32 alignment)
	{
		return MEMPTR<void>(PPCCoreCallback(gCoreinitData->MEMAllocFromDefaultHeapEx.GetMPTR(), size, alignment)).GetPtr();
	}

	void* _weak_MEMAllocFromDefaultHeap(uint32 size)
	{
		return MEMPTR<void>(PPCCoreCallback(gCoreinitData->MEMAllocFromDefaultHeap.GetMPTR(), size)).GetPtr();
	}

	void _weak_MEMFreeToDefaultHeap(void* ptr)
	{
		PPCCoreCallback(gCoreinitData->MEMFreeToDefaultHeap.GetMPTR(), ptr);
	}

	/* MEMAllocator */

	// MEMAllocFromAllocator(allocator, size) / MEMFreeToAllocator(allocator, ptr) jump straight into the
	// allocator's function with the arguments and return address untouched
	static void GuestAllocFromAllocator(PPCInterpreter_t* hCPU)
	{
		MEMAllocator* allocator = MEMPTR<MEMAllocator>(hCPU->gpr[3]).GetPtr();
		hCPU->instructionPointer = allocator->func->funcAlloc.GetMPTR();
	}

	static void GuestFreeToAllocator(PPCInterpreter_t* hCPU)
	{
		MEMAllocator* allocator = MEMPTR<MEMAllocator>(hCPU->gpr[3]).GetPtr();
		hCPU->instructionPointer = allocator->func->funcFree.GetMPTR();
	}

	// default heap allocator functions: drop the allocator argument and continue in the default heap function
	static void DefaultHeapAllocatorAlloc(PPCInterpreter_t* hCPU)
	{
		hCPU->gpr[3] = hCPU->gpr[4];
		hCPU->instructionPointer = gCoreinitData->MEMAllocFromDefaultHeap.GetMPTR();
	}

	static void DefaultHeapAllocatorFree(PPCInterpreter_t* hCPU)
	{
		hCPU->gpr[3] = hCPU->gpr[4];
		hCPU->instructionPointer = gCoreinitData->MEMFreeToDefaultHeap.GetMPTR();
	}

	SysAllocator<MEMAllocatorFunc> g_defaultHeapAllocatorFuncs;

	void CoreInitDefaultHeap()
	{
		cemu_assert_unimplemented();
	}

	void InitDefaultHeaps(MEMPTR<MEMHeapBase>& mem1Heap, MEMPTR<MEMHeapBase>& memFGHeap, MEMPTR<MEMHeapBase>& mem2Heap)
	{
		mem1Heap = nullptr;
		memFGHeap = nullptr;
		mem2Heap = nullptr;

		gCoreinitData->MEMAllocFromDefaultHeap = RPLLoader_MakePPCCallable(GuestDefaultAlloc);
		gCoreinitData->MEMAllocFromDefaultHeapEx = RPLLoader_MakePPCCallable(GuestDefaultAllocEx);
		gCoreinitData->MEMFreeToDefaultHeap = RPLLoader_MakePPCCallable(GuestDefaultFree);

		MEMPTR<void> base;
		uint32be size;
		if (OSGetForegroundBucket(nullptr, nullptr))
		{
			// MEM1 and the free part of the foreground bucket become frame heaps
			OSGetMemBound(1, &base, &size);
			mem1Heap = MEMCreateFrmHeapEx(base.GetPtr(), size, 0);
			OSGetForegroundBucketFreeArea(&base, &size);
			memFGHeap = MEMCreateFrmHeapEx(base.GetPtr(), size, 0);
		}
		// MEM2 becomes the thread-safe default expanded heap
		OSGetMemBound(2, &base, &size);
		g_mem.defaultHeap = MEMCreateExpHeapEx(base.GetPtr(), size, MEM_HEAP_OPTION_THREADSAFE);
		cemu_assert_debug(g_mem.defaultHeap);
		mem2Heap = g_mem.defaultHeap;

		OSDynLoad_SetAllocator(RPLLoader_MakePPCCallable(GuestDynLoadAlloc), RPLLoader_MakePPCCallable(GuestDynLoadFree));
		OSDynLoad_SetTLSAllocator(RPLLoader_MakePPCCallable(GuestDynLoadAlloc), RPLLoader_MakePPCCallable(GuestDynLoadFree));
	}

	void InitializeMEM()
	{
		// forget everything from the previous title
		g_mem.baseHeaps.fill(nullptr);
		g_mem.rootsReady = false;
		g_mem.globalLockReady = false;
		g_mem.defaultHeap = nullptr;
		memset(&g_mem.rootOther, 0, sizeof(MEMList));
		memset(&g_mem.rootMem1, 0, sizeof(MEMList));
		memset(&g_mem.rootForeground, 0, sizeof(MEMList));

		cafeExportRegister("coreinit", CoreInitDefaultHeap, LogType::CoreinitMem);

		osLib_addFunction("coreinit", "MEMInitAllocatorForDefaultHeap", coreinitExport_MEMInitAllocatorForDefaultHeap);
		osLib_addFunction("coreinit", "MEMAllocFromAllocator", GuestAllocFromAllocator);
		osLib_addFunction("coreinit", "MEMFreeToAllocator", GuestFreeToAllocator);

		cafeExportRegister("coreinit", MEMGetBaseHeapHandle, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMSetBaseHeapHandle, LogType::CoreinitMem);

		cafeExportRegister("coreinit", MEMFindContainHeap, LogType::CoreinitMem);

		cafeExportRegister("coreinit", MEMGetFillValForHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMSetFillValForHeap, LogType::CoreinitMem);

		cafeExportRegister("coreinit", MEMCreateUserHeapHandle, LogType::CoreinitMem);

		cafeExportRegister("coreinit", MEMInitList, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMPrependListObject, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMAppendListObject, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMRemoveListObject, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetNextListObject, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetNthListObject, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetPrevListObject, LogType::CoreinitMem);
	}
}

// MEMInitAllocatorForDefaultHeap(MEMAllocator* allocator)
void coreinitExport_MEMInitAllocatorForDefaultHeap(PPCInterpreter_t* hCPU)
{
	using namespace coreinit;
	MEMAllocator* allocator = MEMPTR<MEMAllocator>(hCPU->gpr[3]).GetPtr();
	g_defaultHeapAllocatorFuncs->funcAlloc = PPCInterpreter_makeCallableExportDepr(DefaultHeapAllocatorAlloc);
	g_defaultHeapAllocatorFuncs->funcFree = PPCInterpreter_makeCallableExportDepr(DefaultHeapAllocatorFree);
	allocator->func = g_defaultHeapAllocatorFuncs.GetPtr();
	allocator->heap = MEMGetBaseHeapHandle(1);
	allocator->param1 = 0;
	allocator->param2 = 0;
	osLib_returnFromFunction(hCPU, 0);
}
