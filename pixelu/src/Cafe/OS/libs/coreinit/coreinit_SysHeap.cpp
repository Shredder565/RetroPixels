// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_SysHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"

/* OSAllocFromSystem / OSFreeToSystem
 * A thread-safe expanded heap in the emulator system area, created once per boot. */

namespace coreinit
{
	namespace
	{
		constexpr uint32 SYSTEM_HEAP_SIZE = 8 * 1024 * 1024;
		MEMHeapHandle s_systemHeap = nullptr;
		uint32 s_systemAllocCount = 0;
		uint32 s_systemFreeCount = 0;
	}

	void* OSAllocFromSystem(uint32 size, uint32 alignment)
	{
		s_systemAllocCount++;
		return MEMAllocFromExpHeapEx(s_systemHeap, size, alignment);
	}

	void OSFreeToSystem(void* ptr)
	{
		s_systemFreeCount++;
		MEMFreeToExpHeap(s_systemHeap, ptr);
	}

	void InitSysHeap()
	{
		void* base = MEMPTR<void>(coreinit_allocFromSysArea(SYSTEM_HEAP_SIZE, 0x1000)).GetPtr();
		s_systemHeap = MEMCreateExpHeapEx(base, SYSTEM_HEAP_SIZE, MEM_HEAP_OPTION_THREADSAFE);
		s_systemAllocCount = 0;
		s_systemFreeCount = 0;
	}

	void InitializeSysHeap()
	{
		cafeExportRegister("coreinit", OSAllocFromSystem, LogType::CoreinitMem);
		cafeExportRegister("coreinit", OSFreeToSystem, LogType::CoreinitMem);
	}
}
