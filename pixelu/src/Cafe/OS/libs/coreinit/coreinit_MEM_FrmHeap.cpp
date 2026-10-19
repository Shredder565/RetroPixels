// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_FrmHeap.h"

/* Frame heap
 * Two cursors: allocationHead moves up from heapStart, allocationTail moves down from heapEnd.
 * Memory is only released in bulk (one side or both), or by rolling back to a state that
 * MEMRecordStateForFrmHeap stored inside the heap. Cursor arithmetic is 32-bit guest
 * arithmetic, like on the console. */

namespace coreinit
{
	namespace
	{
		bool CheckFrmHeap(MEMFrmHeap* heap, const char* caller)
		{
			if (!heap)
			{
				cemuLog_log(LogType::APIErrors, "{}: heap is null", caller);
				return false;
			}
			if (heap->magic != MEMHeapMagic::FRAME_HEAP)
			{
				cemuLog_log(LogType::APIErrors, "{}: not a frame heap", caller);
				return false;
			}
			return true;
		}

		void ZeroGuest(uint32 addr, uint32 length)
		{
			memset(MEMPTR<uint8>(addr).GetPtr(), 0, length);
		}

		uint32 AlignMaskOf(sint32 alignment)
		{
			return ~((uint32)alignment - 1);
		}

		void* TakeFromHead(MEMFrmHeap* heap, uint32 size, sint32 alignment)
		{
			const uint32 oldHead = heap->allocationHead.GetMPTR();
			const uint32 start = (oldHead + (uint32)alignment - 1) & AlignMaskOf(alignment);
			if (start + size > heap->allocationTail.GetMPTR())
				return nullptr;
			heap->allocationHead = MEMPTR<void>(start + size);
			if (heap->HasOptionClear())
				ZeroGuest(oldHead, start + size - oldHead);
			return MEMPTR<void>(start).GetPtr();
		}

		void* TakeFromTail(MEMFrmHeap* heap, uint32 size, sint32 alignment)
		{
			const uint32 oldTail = heap->allocationTail.GetMPTR();
			const uint32 start = (oldTail - size) & AlignMaskOf(alignment);
			if (start < heap->allocationHead.GetMPTR())
				return nullptr;
			heap->allocationTail = MEMPTR<void>(start);
			if (heap->HasOptionClear())
				ZeroGuest(start, oldTail - start);
			return MEMPTR<void>(start).GetPtr();
		}
	}

	MEMFrmHeap* MEMCreateFrmHeapEx(void* memStart, uint32 size, uint32 createFlags)
	{
		cemu_assert_debug(memStart);
		const uint64 rawStart = MEMPTR<void>(memStart).GetMPTR();
		const uint64 begin = (rawStart + 3) & ~3ull;
		const uint64 end = (rawStart + size) & ~3ull;
		if (begin == 0 || begin > end || end - begin < sizeof(MEMFrmHeap))
			return nullptr;
		MEMFrmHeap* heap = (MEMFrmHeap*)MEMPTR<void>((uint32)begin).GetPtr();
		MEMInitHeapBase(heap, MEMHeapMagic::FRAME_HEAP, MEMPTR<void>((uint32)begin + sizeof(MEMFrmHeap)).GetPtr(), MEMPTR<void>((uint32)end).GetPtr(), createFlags);
		heap->allocationHead = heap->heapStart;
		heap->allocationTail = heap->heapEnd;
		heap->recordedStates = nullptr;
		return heap;
	}

	void* MEMDestroyFrmHeap(MEMFrmHeap* frmHeap)
	{
		if (!CheckFrmHeap(frmHeap, "MEMDestroyFrmHeap"))
			return nullptr;
		MEMBaseDestroyHeap(frmHeap);
		return frmHeap;
	}

	// space left between the cursors after aligning the head cursor; the sign of the alignment is ignored
	uint32 MEMGetAllocatableSizeForFrmHeapEx(MEMFrmHeap* frmHeap, sint32 alignment)
	{
		if (!CheckFrmHeap(frmHeap, "MEMGetAllocatableSizeForFrmHeapEx"))
			return 0;
		const uint32 align = alignment < 0 ? (0u - (uint32)alignment) : (uint32)alignment;
		if (align == 0 || (align & (align - 1)) != 0)
		{
			cemuLog_log(LogType::APIErrors, "MEMGetAllocatableSizeForFrmHeapEx(): invalid alignment {}", alignment);
			return 0;
		}
		frmHeap->AcquireLock();
		const uint32 start = (frmHeap->allocationHead.GetMPTR() + align - 1) & ~(align - 1);
		const uint32 tail = frmHeap->allocationTail.GetMPTR();
		const uint32 result = start <= tail ? tail - start : 0;
		frmHeap->ReleaseLock();
		return result;
	}

	void* MEMiGetFreeStartForFrmHeap(MEMFrmHeap* heap)
	{
		if (!CheckFrmHeap(heap, "MEMiGetFreeStartForFrmHeap"))
			return nullptr;
		return heap->allocationHead.GetPtr();
	}

	void* MEMiGetFreeEndForFrmHeap(MEMFrmHeap* heap)
	{
		if (!CheckFrmHeap(heap, "MEMiGetFreeEndForFrmHeap"))
			return nullptr;
		return heap->allocationTail.GetPtr();
	}

	// positive alignment allocates from the head, negative alignment from the tail
	void* MEMAllocFromFrmHeapEx(MEMFrmHeap* frmHeap, uint32 size, sint32 alignment)
	{
		if (!CheckFrmHeap(frmHeap, "MEMAllocFromFrmHeapEx"))
			return nullptr;
		if (size == 0)
			size = 4;
		size = (size + 3) & ~3u;
		frmHeap->AcquireLock();
		void* mem;
		if (alignment < 0)
			mem = TakeFromTail(frmHeap, size, (sint32)(0u - (uint32)alignment));
		else
			mem = TakeFromHead(frmHeap, size, alignment);
		frmHeap->ReleaseLock();
		return mem;
	}

	void MEMFreeToFrmHeap(MEMFrmHeap* frmHeap, FrmHeapMode mode)
	{
		if (!CheckFrmHeap(frmHeap, "MEMFreeToFrmHeap"))
			return;
		frmHeap->AcquireLock();
		// releasing either side invalidates all recorded states
		if ((mode & FrmHeapMode::Head) != 0)
		{
			frmHeap->recordedStates = nullptr;
			frmHeap->allocationHead = frmHeap->heapStart;
		}
		if ((mode & FrmHeapMode::Tail) != 0)
		{
			frmHeap->recordedStates = nullptr;
			frmHeap->allocationTail = frmHeap->heapEnd;
		}
		frmHeap->ReleaseLock();
	}

	// stores the current cursors in a 0x10 byte record allocated from the head
	bool MEMRecordStateForFrmHeap(MEMFrmHeap* frmHeap, uint32 id)
	{
		if (!CheckFrmHeap(frmHeap, "MEMRecordStateForFrmHeap"))
			return false;
		frmHeap->AcquireLock();
		const MEMPTR<void> head = frmHeap->allocationHead;
		const MEMPTR<void> tail = frmHeap->allocationTail;
		auto* record = (MEMFrmHeapRecordedState*)TakeFromHead(frmHeap, sizeof(MEMFrmHeapRecordedState), 4);
		if (record)
		{
			record->id = id;
			record->allocationHead = head;
			record->allocationTail = tail;
			record->prevRecordedState = frmHeap->recordedStates;
			frmHeap->recordedStates = record;
		}
		frmHeap->ReleaseLock();
		return record != nullptr;
	}

	// rolls back to the newest state with the given id (id 0: the newest state)
	bool MEMFreeByStateToFrmHeap(MEMFrmHeap* frmHeap, uint32 id)
	{
		if (!CheckFrmHeap(frmHeap, "MEMFreeByStateToFrmHeap"))
			return false;
		frmHeap->AcquireLock();
		MEMFrmHeapRecordedState* record = frmHeap->recordedStates.GetPtr();
		while (record && id != 0 && record->id != id)
			record = record->prevRecordedState.GetPtr();
		if (record)
		{
			frmHeap->allocationHead = record->allocationHead;
			frmHeap->allocationTail = record->allocationTail;
			frmHeap->recordedStates = record->prevRecordedState;
		}
		frmHeap->ReleaseLock();
		return record != nullptr;
	}

	void InitializeMEMFrmHeap()
	{
		cafeExportRegister("coreinit", MEMCreateFrmHeapEx, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMDestroyFrmHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMGetAllocatableSizeForFrmHeapEx, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMiGetFreeStartForFrmHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMiGetFreeEndForFrmHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMAllocFromFrmHeapEx, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMFreeToFrmHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMRecordStateForFrmHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMFreeByStateToFrmHeap, LogType::CoreinitMem);
	}
}
