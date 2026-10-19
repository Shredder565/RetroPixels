// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_UnitHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_Spinlock.h"

/* Unit heap
 * The heap header is followed (after alignment) by an array of equally sized units. Free units
 * are chained through their first word; allocation pops the first free unit and freeing pushes
 * the unit back to the front. */

namespace coreinit
{
	namespace
	{
		void CheckUnitHeap(MEMHeapHandle heap)
		{
			cemu_assert(heap != MEM_HEAP_INVALID_HANDLE);
			cemu_assert(heap->magic == MEMHeapMagic::UNIT_HEAP);
		}

		uint32 RoundUpUnit(uint32 size, uint32 alignment)
		{
			return (size + alignment - 1) & ~(alignment - 1);
		}
	}

	MEMHeapBase* MEMCreateUnitHeapEx(void* memStart, uint32 heapSize, uint32 blockSize, uint32 alignment, uint32 createFlags)
	{
		cemu_assert_debug(memStart != nullptr);
		cemu_assert_debug((alignment & 3) == 0 && alignment >= 4 && alignment <= 32);

		const uint64 rawStart = MEMPTR<void>(memStart).GetMPTR();
		const uint64 begin = (rawStart + 3) & ~3ull;
		const uint64 end = (rawStart + heapSize) & ~3ull;
		if (begin > end)
			return nullptr;
		const uint64 alignMinusOne = (uint32)(alignment - 1);
		const uint64 firstUnit = (begin + sizeof(MEMUnitHeap) + alignMinusOne) & ~alignMinusOne;
		if (firstUnit > end)
			return nullptr;
		blockSize = RoundUpUnit(blockSize, alignment);
		if (blockSize == 0)
			return nullptr;
		const uint32 unitCount = (uint32)(end - firstUnit) / blockSize;
		if (unitCount == 0)
			return nullptr;

		MEMUnitHeap* heap = (MEMUnitHeap*)MEMPTR<void>((uint32)begin).GetPtr();
		const uint32 unitsBegin = (uint32)firstUnit;
		MEMInitHeapBase(heap, MEMHeapMagic::UNIT_HEAP, MEMPTR<void>(unitsBegin).GetPtr(), MEMPTR<void>(unitsBegin + unitCount * blockSize).GetPtr(), createFlags);
		heap->firstFreeBlock = MEMPTR<MEMUnitHeapBlock>(unitsBegin);
		heap->blockSize = blockSize;
		// chain every unit to the one after it
		for (uint32 i = 0; i < unitCount; i++)
		{
			MEMUnitHeapBlock* unit = MEMPTR<MEMUnitHeapBlock>(unitsBegin + i * blockSize).GetPtr();
			if (i + 1 < unitCount)
				unit->nextBlock = MEMPTR<MEMUnitHeapBlock>(unitsBegin + (i + 1) * blockSize);
			else
				unit->nextBlock = nullptr;
		}
		return heap;
	}

	void* MEMDestroyUnitHeap(MEMHeapHandle heap)
	{
		CheckUnitHeap(heap);
		MEMBaseDestroyHeap(heap);
		return heap;
	}

	// heap size needed for blockCount units of blockSize bytes with the given alignment
	uint32 MEMCalcHeapSizeForUnitHeap(uint32 blockSize, uint32 blockCount, uint32 alignment)
	{
		return blockCount * RoundUpUnit(blockSize, alignment) + (alignment - 4) + sizeof(MEMUnitHeap);
	}

	uint32 MEMCountFreeBlockForUnitHeap(MEMUnitHeap* heap)
	{
		CheckUnitHeap(heap);
		heap->AcquireLock();
		uint32 count = 0;
		for (MEMUnitHeapBlock* unit = heap->firstFreeBlock.GetPtr(); unit; unit = unit->nextBlock.GetPtr())
			count++;
		heap->ReleaseLock();
		return count;
	}

	void* MEMAllocFromUnitHeap(MEMUnitHeap* heap)
	{
		CheckUnitHeap(heap);
		heap->AcquireLock();
		MEMUnitHeapBlock* unit = heap->firstFreeBlock.GetPtr();
		if (unit)
		{
			heap->firstFreeBlock = unit->nextBlock;
			if (heap->HasOptionClear())
				memset(unit, 0, heap->blockSize);
			else if (heap->HasOptionFill())
				memset(unit, (int)MEMGetFillValForHeap(HEAP_FILL_TYPE::ON_ALLOC), heap->blockSize);
		}
		heap->ReleaseLock();
		return unit;
	}

	void MEMFreeToUnitHeap(MEMUnitHeap* heap, void* mem)
	{
		CheckUnitHeap(heap);
		if (!mem)
			return;
		heap->AcquireLock();
		cemu_assert_debug(mem >= heap->heapStart.GetPtr() && mem < heap->heapEnd.GetPtr());
		MEMUnitHeapBlock* unit = (MEMUnitHeapBlock*)mem;
		// fill first, the link word must survive
		if (heap->HasOptionFill())
			memset(unit, (int)MEMGetFillValForHeap(HEAP_FILL_TYPE::ON_FREE), heap->blockSize);
		unit->nextBlock = heap->firstFreeBlock;
		heap->firstFreeBlock = unit;
		heap->ReleaseLock();
	}

	void InitializeMEMUnitHeap()
	{
		cafeExportRegister("coreinit", MEMCreateUnitHeapEx, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMDestroyUnitHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMCalcHeapSizeForUnitHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMCountFreeBlockForUnitHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMAllocFromUnitHeap, LogType::CoreinitMem);
		cafeExportRegister("coreinit", MEMFreeToUnitHeap, LogType::CoreinitMem);
	}
}
