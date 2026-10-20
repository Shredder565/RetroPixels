// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// coreinit module entry: registers the core-identification exports, wires up
// all coreinit sub-systems when the module is mapped and starts the title's
// main thread once the module is loaded. Also hosts the guest stack-trace
// logger and OSGetSharedData (system fonts).
#include "Cafe/OS/common/OSCommon.h"
#include "Common/SysAllocator.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"

#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"

#include "Cafe/OS/libs/coreinit/coreinit_BSP.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Atomic.h"
#include "Cafe/OS/libs/coreinit/coreinit_OverlayArena.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "Cafe/OS/libs/coreinit/coreinit_GHS.h"
#include "Cafe/OS/libs/coreinit/coreinit_HWInterface.h"
#include "Cafe/OS/libs/coreinit/coreinit_Memory.h"
#include "Cafe/OS/libs/coreinit/coreinit_IM.h"
#include "Cafe/OS/libs/coreinit/coreinit_LockedCache.h"
#include "Cafe/OS/libs/coreinit/coreinit_MemoryMapping.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPC.h"
#include "Cafe/OS/libs/coreinit/coreinit_IPCBuf.h"
#include "Cafe/OS/libs/coreinit/coreinit_Coroutine.h"
#include "Cafe/OS/libs/coreinit/coreinit_OSScreen.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"
#include "Cafe/OS/libs/coreinit/coreinit_SystemInfo.h"
#include "Cafe/OS/libs/coreinit/coreinit_SysHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MCP.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_MPQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_UnitHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_FrmHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_BlockHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"

CoreinitSharedData* gCoreinitData = nullptr;

uint8* extractCafeDefaultFont(sint32* size); // resource/CafeDefaultFont.cpp

namespace
{
	// PPC EABI frame: word 0 is the back chain, word 1 the saved LR of the caller frame
	struct GuestStackBounds
	{
		uint32 low;
		uint32 high;
		bool Contains(uint32 addr) const { return addr >= low && addr <= high; }
	};

	GuestStackBounds StackBoundsOf(OSThread_t* thread)
	{
		return { thread->stackEnd.GetMPTR(), thread->stackBase.GetMPTR() };
	}

	// How plausible is it that a valid back-chain starts at sp? Walks up to 50
	// frames; every well-formed link scores, return addresses inside the code
	// region add a bonus and implausible ones a penalty.
	sint32 RateFrameChain(const GuestStackBounds& stack, uint32 sp)
	{
		sint32 rating = 0;
		uint32 frame = sp;
		for (sint32 depth = 0; depth < 50; depth++)
		{
			const uint32 caller = memory_readU32(frame);
			if (caller < frame || !stack.Contains(caller) || (caller & 3) != 0)
				break;
			rating += 10;
			const uint32 lr = memory_readU32(caller + 4);
			const bool lrLooksLikeCode = lr != 0 && lr < 0x10000000 && (lr & 3) == 0;
			rating += lrLooksLikeCode ? 5 : -5;
			frame = caller;
		}
		return rating;
	}
}

// r1 may be mid-prologue, so try the next 256 words and log the chain that looks best
void DebugLogStackTrace(OSThread_t* thread, MPTR sp)
{
	const GuestStackBounds stack = StackBoundsOf(thread);
	uint32 start = sp;
	sint32 bestRating = -1;
	for (uint32 word = 0; word < 0x100; word++)
	{
		const uint32 candidate = sp + word * 4;
		const sint32 rating = RateFrameChain(stack, candidate);
		if (rating > bestRating)
		{
			bestRating = rating;
			start = candidate;
		}
	}

	cemuLog_log(LogType::Force, "Guest stack trace (r1={:08x}, chain starts at {:08x})", sp, start);
	uint32 frame = start;
	for (sint32 depth = 0; depth < 20; depth++)
	{
		const uint32 caller = memory_readU32(frame);
		if (caller < frame || !stack.Contains(caller))
			break;
		const uint32 lr = memory_readU32(caller + 4);
		if (RPLStoredSymbol* sym = rplSymbolStorage_getByClosestAddress(lr))
			cemuLog_log(LogType::Force, "  frame {:08x} lr {:08x}  {}.{}+0x{:x}", caller, lr, (const char*)sym->libName, (const char*)sym->symbolName, lr - sym->address);
		else
			cemuLog_log(LogType::Force, "  frame {:08x} lr {:08x}", caller, lr);
		frame = caller;
	}
}

namespace coreinit
{
	namespace
	{
		// One record of the shared-data table the system places at MEMORY_SHAREDDATA_AREA_ADDR.
		// The table holds the four system fonts, tagged 0xFFCAFE01..0xFFCAFE04.
		struct SharedDataRecord
		{
			/* +0x00 */ uint32be tag;
			/* +0x04 */ uint32be kind;          // 2 = font
			/* +0x08 */ uint32be kernelPath;
			/* +0x0C */ MEMPTR<void> data;
			/* +0x10 */ uint32be size;
			/* +0x14 */ uint32be reserved14;
			/* +0x18 */ uint32be reserved18;
		};
		static_assert(sizeof(SharedDataRecord) == 0x1C);

		constexpr uint32 kSharedDataTagBase = 0xFFCAFE01;
		constexpr sint32 kSharedDataRecordCount = 4;

		// substitute for missing system fonts, loaded on first request
		MPTR s_fallbackFont = MPTR_NULL;
		sint32 s_fallbackFontSize = 0;

		// OSGetSharedData(id, flags, void** dataOut, uint32* sizeOut) -> BOOL
		void export_OSGetSharedData(PPCInterpreter_t* hCPU)
		{
			const uint32 id = hCPU->gpr[3];
			const uint32 dataOut = hCPU->gpr[5];
			const uint32 sizeOut = hCPU->gpr[6];

			const auto* table = (const SharedDataRecord*)memory_getPointerFromVirtualOffset(MEMORY_SHAREDDATA_AREA_ADDR);
			const uint32 wanted = kSharedDataTagBase + id;
			for (sint32 i = 0; i < kSharedDataRecordCount; i++)
			{
				if (table[i].tag != wanted)
					continue;
				memory_writeU32(dataOut, table[i].data.GetMPTR());
				memory_writeU32(sizeOut, table[i].size);
				osLib_returnFromFunction(hCPU, 1);
				return;
			}

			// No system fonts are installed. Titles tend to crash on failure, so
			// hand out the bundled default font for every request instead.
			cemuLog_log(LogType::Force, "OSGetSharedData: system fonts not available, returning built-in font");
			if (s_fallbackFont == MPTR_NULL)
			{
				uint8* font = extractCafeDefaultFont(&s_fallbackFontSize);
				s_fallbackFont = coreinit_allocFromSysArea(s_fallbackFontSize, 256);
				if (s_fallbackFont == MPTR_NULL)
					cemuLog_log(LogType::Force, "OSGetSharedData: out of system memory for built-in font");
				memcpy(memory_getPointerFromVirtualOffset(s_fallbackFont), font, s_fallbackFontSize);
				free(font);
			}
			memory_writeU32(dataOut, s_fallbackFont);
			memory_writeU32(sizeOut, s_fallbackFontSize);
			osLib_returnFromFunction(hCPU, 1);
		}

		// ENVGetEnvironmentVariable(name, buffer, bufferSize): no variables exist,
		// so the buffer gets an empty string
		void export_ENVGetEnvironmentVariable(PPCInterpreter_t* hCPU)
		{
			const char* name = (const char*)memory_getPointerFromVirtualOffset(hCPU->gpr[3]);
			char* buffer = (char*)memory_getPointerFromVirtualOffset(hCPU->gpr[4]);
			const sint32 bufferSize = (sint32)hCPU->gpr[5];
			cemuLog_logDebug(LogType::Force, "ENVGetEnvironmentVariable(\"{}\", 0x{:08x}, 0x{:x})", name, hCPU->gpr[4], hCPU->gpr[5]);
			if (bufferSize > 0)
				buffer[0] = '\0';
			osLib_returnFromFunction(hCPU, 1);
		}
	}

	sint32 OSGetCoreId()
	{
		return PPCInterpreter_getCoreIndex(PPCInterpreter_getCurrentInstance());
	}

	uint32 OSGetCoreCount()
	{
		return Espresso::CORE_COUNT;
	}

	uint32 OSIsDebuggerInitialized()
	{
		return 0;
	}

	uint32 OSIsDebuggerPresent()
	{
		return 0;
	}

	uint32 OSGetConsoleType()
	{
		return 0x03000050; // retail console
	}

	uint32 OSGetMainCoreId()
	{
		return 1;
	}

	bool OSIsMainCore()
	{
		return OSGetCoreId() == (sint32)OSGetMainCoreId();
	}

	uint32 OSGetStackPointer()
	{
		return PPCInterpreter_getCurrentInstance()->gpr[1];
	}

	bool OSIsOffBoot()
	{
		return true;
	}

	uint32 OSGetBootPMFlags()
	{
		cemuLog_logDebug(LogType::Force, "OSGetBootPMFlags: not implemented, returning 0");
		return 0;
	}

	uint32 OSGetSystemMode()
	{
		// must stay 0: Barista hangs soon after boot when it sees 2
		cemuLog_logDebug(LogType::Force, "OSGetSystemMode: not implemented, returning 0");
		return 0;
	}

	void OSPanic(const char* file, sint32 lineNumber, const char* msg)
	{
		cemuLog_log(LogType::Force, "OSPanic at {}:{}: {}", file, lineNumber, msg);
		DebugLogStackTrace(OSGetCurrentThread(), OSGetStackPointer());
#ifdef CEMU_DEBUG_ASSERT
		// keep the guest thread parked so the state can be inspected
		for (;;)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
	}

	void InitializeCore()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::CoreinitThread)
		EXPORT_FN(OSGetCoreId); EXPORT_FN(OSGetCoreCount); EXPORT_FN(OSIsDebuggerInitialized);
		EXPORT_FN(OSIsDebuggerPresent); EXPORT_FN(OSGetConsoleType); EXPORT_FN(OSGetMainCoreId);
		EXPORT_FN(OSIsMainCore); EXPORT_FN(OSGetStackPointer);
#undef EXPORT_FN

		osLib_addFunction("coreinit", "ENVGetEnvironmentVariable", export_ENVGetEnvironmentVariable);

		cafeExportRegister("coreinit", OSIsOffBoot, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetBootPMFlags, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetSystemMode, LogType::CoreinitThread);

		cafeExportRegister("coreinit", OSPanic, LogType::Placeholder);
	}

	namespace
	{
		void PublishSharedData()
		{
			gCoreinitData = (CoreinitSharedData*)memory_getPointerFromVirtualOffset(coreinit_allocFromSysArea(sizeof(CoreinitSharedData), 32));
			memset((void*)gCoreinitData, 0, sizeof(CoreinitSharedData));

			const auto publish = [](const char* name, void* field) {
				osLib_addVirtualPointer("coreinit", name, memory_getVirtualOffsetFromPointer(field));
			};
			publish("MEMAllocFromDefaultHeap", &gCoreinitData->MEMAllocFromDefaultHeap);
			publish("MEMAllocFromDefaultHeapEx", &gCoreinitData->MEMAllocFromDefaultHeapEx);
			publish("MEMFreeToDefaultHeap", &gCoreinitData->MEMFreeToDefaultHeap);
			publish("__atexit_cleanup", &gCoreinitData->__atexit_cleanup);
			publish("__stdio_cleanup", &gCoreinitData->__stdio_cleanup);
			publish("__cpp_exception_cleanup_ptr", &gCoreinitData->__cpp_exception_cleanup_ptr);
			publish("__cpp_exception_init_ptr", &gCoreinitData->__cpp_exception_init_ptr);
		}

		class CoreinitModule : public COSModule
		{
		public:
			std::string_view GetName() override
			{
				return "coreinit";
			}

			void RPLMapped() override
			{
				InitializeCore();
				InitializeSchedulerLock();
				InitializeSysHeap();
				PublishSharedData();

				PrepareGHSRuntime();
				MapThreadExports();
				activeThreadCount = 0;

				// the order below fixes export registration and system-area layout
				InitializeMEM();
				InitializeMEMFrmHeap();
				InitializeMEMUnitHeap();
				InitializeMEMBlockHeap();
				InitializeFG();
				InitializeBSP();
				InitializeMCP();
				InitializeOverlayArena();
				InitializeDynLoad();
				InitializeGHS();
				InitializeHWInterface();
				InitializeAtomic();
				InitializeMemory();
				InitializeIM();
				InitializeLC();
				InitializeMP();
				InitializeTimeAndCalendar();
				MapAlarmExports();
				InitializeFS();
				InitializeSystemInfo();
				InitializeConcurrency();
				InitializeSpinlock();
				InitializeMessageQueue();
				MapIPCExports();
				InitializeIPCBuf();
				InitializeMemoryMapping();
				InitializeCodeGen();
				InitializeCoroutine();
				InitializeOSScreen();

				expheap_load(); // older expanded-heap exports

				miscInit();
				osLib_addFunction("coreinit", "OSGetSharedData", export_OSGetSharedData);
				osLib_addFunction("coreinit", "UCReadSysConfig", coreinitExport_UCReadSysConfig);
			}

			void rpl_entry(uint32 /*moduleHandle*/, RplEntryReason reason) override
			{
				if (reason != RplEntryReason::Loaded)
					return; // unloading coreinit is not supported
				InitializeThread();
				InitializeAlarm();
				InitializeIPC();
				InitializeAsyncCallback();
				// the rest of start-up (coreinit_start) needs a guest context, so it
				// runs as the first code of the main core's default thread
				OSThread_t* mainThread = OSGetDefaultThread(1);
				OSSetThreadPriority(mainThread, 16);
				OSRunThread(mainThread, PPCInterpreter_makeCallableExportDepr(coreinit_start), 0, nullptr);
			}
		};

		CoreinitModule s_module;
	}

	COSModule* GetModule()
	{
		return &s_module;
	}
}
