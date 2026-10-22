// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Espresso/Const.h"
#include "Cafe/OS/RPL/COSModule.h"

#define PPC_CORE_COUNT     (Espresso::CORE_COUNT)

#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"

// Deferred guest calls, run on a dedicated guest thread (coreinit_Callbacks.cpp)
void InitializeAsyncCallback();
void coreinitAsyncCallback_add(MPTR functionMPTR, uint32 numParameters, uint32 r3 = 0, uint32 r4 = 0, uint32 r5 = 0, uint32 r6 = 0, uint32 r7 = 0, uint32 r8 = 0, uint32 r9 = 0, uint32 r10 = 0);
void coreinitAsyncCallback_addWithLock(MPTR functionMPTR, uint32 numParameters, uint32 r3 = 0, uint32 r4 = 0, uint32 r5 = 0, uint32 r6 = 0, uint32 r7 = 0, uint32 r8 = 0, uint32 r9 = 0, uint32 r10 = 0);

// Guest-resident words that coreinit exports as data symbols. Titles and the
// C runtime overwrite them to install their own allocator and cleanup hooks.
struct CoreinitSharedData
{
	/* +0x00 */ MEMPTR<void> MEMAllocFromDefaultHeap{nullptr};
	/* +0x04 */ MEMPTR<void> MEMAllocFromDefaultHeapEx{nullptr};
	/* +0x08 */ MEMPTR<void> MEMFreeToDefaultHeap{nullptr};
	/* +0x0C */ MEMPTR<void> __atexit_cleanup{nullptr};
	/* +0x10 */ MEMPTR<void> __stdio_cleanup{nullptr};
	/* +0x14 */ MPTR __cpp_exception_init_ptr{0};    // stored big-endian
	/* +0x18 */ MPTR __cpp_exception_cleanup_ptr{0}; // stored big-endian
};
static_assert(sizeof(CoreinitSharedData) == 0x1C);

extern CoreinitSharedData* gCoreinitData;

// first guest code run on the main core; ends by jumping to the title entry point
void coreinit_start(PPCInterpreter_t* hCPU);

// raw system-area allocation
MPTR OSAllocFromSystem(uint32 size, uint32 alignment);
void OSFreeToSystem(MPTR mem);

namespace coreinit
{
	sint32 OSGetCoreId();
	uint32 OSGetCoreCount();
	uint32 OSGetStackPointer();

	COSModule* GetModule();
};
