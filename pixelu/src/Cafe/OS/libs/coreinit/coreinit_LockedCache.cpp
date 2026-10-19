// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"

// Locked cache (LC): each core has 16KB of L1 data cache that can be locked and used as fast
// scratch memory, filled and flushed with DMA. Every core sees its own LC at a fixed address:
//   core 0: 0xFFC00000, core 1: 0xFFC40000, core 2: 0xFFC80000
// LCAlloc hands out pieces in 512 byte units. We track the units per core with one state byte each;
// the last unit of an allocation is marked so LCDealloc knows where it ends. DMA is done instantly.
namespace coreinit
{
	namespace
	{
		constexpr uint32 kLCSize = 0x4000;
		constexpr uint32 kLCUnit = 0x200;
		constexpr uint32 kLCUnits = kLCSize / kLCUnit;
		constexpr uint32 kDMABlockSize = 32;
		constexpr uint32 kDMAMaxBlocks = 128; // a block count of 0 means 128

		enum : uint8
		{
			UNIT_FREE = 0,
			UNIT_USED = 1,
			UNIT_LAST = 2, // last unit of an allocation
		};

		constexpr MPTR kLCBase[PPC_CORE_COUNT] = { 0xFFC00000, 0xFFC40000, 0xFFC80000 };

		uint8 s_units[PPC_CORE_COUNT][kLCUnits] = {};
		uint32 s_usedUnits[PPC_CORE_COUNT] = {};
		uint32 s_dmaEnableCount[PPC_CORE_COUNT] = {};
		sint32 s_leakWarnings = 0;

		uint32 CurrentCore(PPCInterpreter_t* hCPU)
		{
			return PPCInterpreter_getCoreIndex(hCPU);
		}

		bool RangeIsFree(const uint8* units, uint32 first, uint32 count)
		{
			for (uint32 i = 0; i < count; i++)
			{
				if (units[first + i] != UNIT_FREE)
					return false;
			}
			return true;
		}

		void Export_LCGetMaxSize(PPCInterpreter_t* hCPU)
		{
			osLib_returnFromFunction(hCPU, kLCSize);
		}

		// LCAlloc(size): first fit, size is rounded up to whole units
		void Export_LCAlloc(PPCInterpreter_t* hCPU)
		{
			uint32 size = hCPU->gpr[3];
			if (size == 0 || size > kLCSize)
			{
				osLib_returnFromFunction(hCPU, MPTR_NULL);
				return;
			}
			size = (size + kLCUnit - 1) & ~(kLCUnit - 1);
			uint32 core = CurrentCore(hCPU);
			uint8* units = s_units[core];
			uint32 count = size / kLCUnit;
			for (uint32 first = 0; first + count <= kLCUnits; first++)
			{
				if (!RangeIsFree(units, first, count))
					continue;
				memset(units + first, UNIT_USED, count - 1);
				units[first + count - 1] = UNIT_LAST;
				s_usedUnits[core] += count;
				osLib_returnFromFunction(hCPU, kLCBase[core] + first * kLCUnit);
				return;
			}
			osLib_returnFromFunction(hCPU, MPTR_NULL);
		}

		// LCDealloc(address): address must be the start of an allocation of the current core
		void Export_LCDealloc(PPCInterpreter_t* hCPU)
		{
			uint32 core = CurrentCore(hCPU);
			uint8* units = s_units[core];
			MPTR addr = hCPU->gpr[3];
			if (addr < kLCBase[core] || addr >= kLCBase[core] + kLCSize)
			{
#ifdef CEMU_DEBUG_ASSERT
				cemuLog_log(LogType::Force, "LCDealloc: {:08x} is not in this core's locked cache", addr);
#endif
				osLib_returnFromFunction(hCPU, 0);
				return;
			}
			uint32 offset = addr - kLCBase[core];
			if (offset % kLCUnit != 0)
			{
				osLib_returnFromFunction(hCPU, 0);
				return;
			}
			uint32 unit = offset / kLCUnit;
			// the unit before an allocation is either free or the last unit of another allocation
			if (unit > 0 && units[unit - 1] == UNIT_USED)
			{
				osLib_returnFromFunction(hCPU, 0);
				return;
			}
			for (; unit < kLCUnits; unit++)
			{
				uint8 state = units[unit];
				if (state == UNIT_FREE)
					break; // not an allocation (or a damaged one)
				units[unit] = UNIT_FREE;
				s_usedUnits[core]--;
				if (state == UNIT_LAST)
					break;
			}
			osLib_returnFromFunction(hCPU, 0);
		}

		void Export_LCGetUnallocated(PPCInterpreter_t* hCPU)
		{
			uint32 core = CurrentCore(hCPU);
			osLib_returnFromFunction(hCPU, kLCSize - s_usedUnits[core] * kLCUnit);
		}

		// largest size LCAlloc can currently succeed with
		void Export_LCGetAllocatableSize(PPCInterpreter_t* hCPU)
		{
			const uint8* units = s_units[CurrentCore(hCPU)];
			uint32 best = 0, run = 0;
			for (uint32 i = 0; i < kLCUnits; i++)
			{
				run = units[i] == UNIT_FREE ? run + 1 : 0;
				best = std::max(best, run);
			}
			osLib_returnFromFunction(hCPU, best * kLCUnit);
		}

		// DMA enable/disable nest per core
		void Export_LCEnableDMA(PPCInterpreter_t* hCPU)
		{
			s_dmaEnableCount[CurrentCore(hCPU)]++;
			osLib_returnFromFunction(hCPU, 1);
		}

		void Export_LCDisableDMA(PPCInterpreter_t* hCPU)
		{
			uint32 core = CurrentCore(hCPU);
			if (s_dmaEnableCount[core] == 0)
				assert_dbg(); // unbalanced disable
			s_dmaEnableCount[core]--;
#ifdef CEMU_DEBUG_ASSERT
			if (s_dmaEnableCount[core] == 0 && s_usedUnits[core] != 0 && s_leakWarnings < 15)
			{
				cemuLog_log(LogType::Force, "LCDisableDMA: locked cache of core {} still has allocations", core);
				s_leakWarnings++;
			}
#endif
			osLib_returnFromFunction(hCPU, 1);
		}

		void Export_LCIsDMAEnabled(PPCInterpreter_t* hCPU)
		{
			osLib_returnFromFunction(hCPU, s_dmaEnableCount[CurrentCore(hCPU)] ? 1 : 0);
		}

		// on the console the LC can be taken by another process; here it is always ours
		void Export_LCHardwareIsAvailable(PPCInterpreter_t* hCPU)
		{
			osLib_returnFromFunction(hCPU, 1);
		}

		uint32 DMABlockCount(uint32 requested)
		{
			return requested == 0 ? kDMAMaxBlocks : requested;
		}

		// LCLoadDMABlocks(lcDest, memSrc, blockCount)
		void Export_LCLoadDMABlocks(PPCInterpreter_t* hCPU)
		{
			uint32 bytes = DMABlockCount(hCPU->gpr[5]) * kDMABlockSize;
			memcpy(memory_getPointerFromVirtualOffset(hCPU->gpr[3]), memory_getPointerFromVirtualOffset(hCPU->gpr[4]), bytes);
			osLib_returnFromFunction(hCPU, 0);
		}

		// LCStoreDMABlocks(memDest, lcSrc, blockCount)
		void Export_LCStoreDMABlocks(PPCInterpreter_t* hCPU)
		{
			uint32 blocks = DMABlockCount(hCPU->gpr[5]);
			memcpy_qwords(memory_getPointerFromVirtualOffset(hCPU->gpr[3]), memory_getPointerFromVirtualOffset(hCPU->gpr[4]), blocks * (kDMABlockSize / sizeof(uint64)));
			// the GPU may have cached the destination
			LatteBufferCache_notifyDCFlush(hCPU->gpr[3], blocks * kDMABlockSize);
			osLib_returnFromFunction(hCPU, 0);
		}

		// transfers complete immediately, there is nothing to wait for
		void Export_LCWaitDMAQueue(PPCInterpreter_t* hCPU)
		{
			osLib_returnFromFunction(hCPU, 0);
		}
	}

	void InitializeLC()
	{
		memset(s_units, UNIT_FREE, sizeof(s_units));

		osLib_addFunction("coreinit", "LCGetMaxSize", Export_LCGetMaxSize);

		osLib_addFunction("coreinit", "LCAlloc", Export_LCAlloc);
		osLib_addFunction("coreinit", "LCDealloc", Export_LCDealloc);
		osLib_addFunction("coreinit", "LCGetUnallocated", Export_LCGetUnallocated);
		osLib_addFunction("coreinit", "LCGetAllocatableSize", Export_LCGetAllocatableSize);

		osLib_addFunction("coreinit", "LCEnableDMA", Export_LCEnableDMA);
		osLib_addFunction("coreinit", "LCDisableDMA", Export_LCDisableDMA);
		osLib_addFunction("coreinit", "LCIsDMAEnabled", Export_LCIsDMAEnabled);
		osLib_addFunction("coreinit", "LCHardwareIsAvailable", Export_LCHardwareIsAvailable);

		osLib_addFunction("coreinit", "LCLoadDMABlocks", Export_LCLoadDMABlocks);
		osLib_addFunction("coreinit", "LCStoreDMABlocks", Export_LCStoreDMABlocks);
		osLib_addFunction("coreinit", "LCWaitDMAQueue", Export_LCWaitDMAQueue);
	}
}
