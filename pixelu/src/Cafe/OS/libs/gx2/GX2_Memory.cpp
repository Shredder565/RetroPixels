// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "GX2.h"
#include "GX2_Resource.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"

// GX2 keeps a pair of guest allocator callbacks (alloc(userParam, size, align) and
// free(userParam, ptr)). By default they forward to the coreinit default heap. The default
// GX2R allocator in turn forwards to whatever the GX2 allocator currently is.
// All forwarders are guest callable stubs that rearrange registers and then tail-jump.

namespace
{
	MPTR s_allocFunc = MPTR_NULL;
	MPTR s_freeFunc = MPTR_NULL;

	// alloc(r3 = userParam, r4 = size, r5 = alignment) -> MEMAllocFromDefaultHeapEx(size, alignment)
	void HeapAllocForwarder(PPCInterpreter_t* hCPU)
	{
		hCPU->gpr[3] = hCPU->gpr[4];
		hCPU->gpr[4] = hCPU->gpr[5];
		hCPU->instructionPointer = gCoreinitData->MEMAllocFromDefaultHeapEx.GetMPTR();
	}

	// free(r3 = userParam, r4 = ptr) -> MEMFreeToDefaultHeap(ptr)
	void HeapFreeForwarder(PPCInterpreter_t* hCPU)
	{
		hCPU->gpr[3] = hCPU->gpr[4];
		hCPU->instructionPointer = gCoreinitData->MEMFreeToDefaultHeap.GetMPTR();
	}

	// GX2R allocations use the same signatures, the arguments are passed through unchanged
	void ResourceAllocForwarder(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::GX2, "GX2DefaultAllocate(0x{:08x}, 0x{:08x}, 0x{:08x})", hCPU->gpr[3], hCPU->gpr[4], hCPU->gpr[5]);
		hCPU->instructionPointer = s_allocFunc;
	}

	void ResourceFreeForwarder(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::GX2, "GX2DefaultFree(0x{:08x}, 0x{:08x})", hCPU->gpr[3], hCPU->gpr[4]);
		hCPU->instructionPointer = s_freeFunc;
	}

	void Export_GX2SetDefaultAllocator(PPCInterpreter_t* hCPU)
	{
		cemuLog_log(LogType::GX2, "GX2SetDefaultAllocator(0x{:08x}, 0x{:08x})", hCPU->gpr[3], hCPU->gpr[4]);
		s_allocFunc = hCPU->gpr[3];
		s_freeFunc = hCPU->gpr[4];
		osLib_returnFromFunction(hCPU, 0);
	}
}

namespace GX2
{
	void GX2MEMAllocatorsInit()
	{
		s_allocFunc = PPCInterpreter_makeCallableExportDepr(HeapAllocForwarder);
		s_freeFunc = PPCInterpreter_makeCallableExportDepr(HeapFreeForwarder);
		const MPTR resourceFree = PPCInterpreter_makeCallableExportDepr(ResourceFreeForwarder);
		const MPTR resourceAlloc = PPCInterpreter_makeCallableExportDepr(ResourceAllocForwarder);
		GX2RSetAllocator(resourceAlloc, resourceFree);
	}

	void GX2MemInit()
	{
		osLib_addFunction("gx2", "GX2SetDefaultAllocator", Export_GX2SetDefaultAllocator);
	}
};
