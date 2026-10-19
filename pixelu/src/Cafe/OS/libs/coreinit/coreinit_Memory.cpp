// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_Memory.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/CafeSystem.h"

// Data cache maintenance, the memory copy/fill exports and memory bounds. Guest memory has no
// cache model; what matters is telling the GPU buffer cache when the CPU may have changed memory
// it mirrors. Flush style operations and all copies do that, invalidate/store do not need to.
namespace coreinit
{
	namespace
	{
		constexpr uint32 kCacheLine = 32;

		// the 32 byte lines covering [addr, addr + size)
		void NotifyLinesWritten(MPTR addr, uint32 size)
		{
			MPTR end = (addr + size + kCacheLine - 1) & ~(kCacheLine - 1);
			MPTR start = addr & ~(kCacheLine - 1);
			LatteBufferCache_notifyDCFlush(start, end - start);
		}

		// Titles that break when they find more free MEM2 than on the console (our system libraries
		// use less memory than the real ones). For these the reported MEM2 bound is moved up by 48MB.
		constexpr uint64 kTitlesNeedingLessMem2[] = {
			0x0005000010132400, 0x0005000010132B00, // LEGO Marvel Super Heroes (EU, US)
			0x0005000010194200, 0x0005000010195D00, // LEGO Dimensions (US, EU)
			0x00050000101A6200, 0x00050000101A5C00, // LEGO Jurassic World (US, EU)
			0x000500001014DE00, 0x000500001014E000, // The LEGO Movie Videogame (US, EU)
			0x0005000010168D00, 0x000500001016A700, // LEGO The Hobbit (EU, JP)
			0x00050000101DAB00, 0x00050000101DAA00, // LEGO Star Wars: The Force Awakens (US, EU)
			0x000500001016A400, 0x000500001016AD00, // LEGO Batman 3: Beyond Gotham (EU, US)
			0x00050000101BE900, 0x00050000101BEF00, // LEGO Marvel's Avengers (EU, US)
			0x0005000010135500, 0x0005000010135E00, // LEGO Batman 2: DC Super Heroes (EU, US)
		};
		constexpr uint32 kMem2Reduction = 48 * 1024 * 1024;

		bool NeedsLessMem2(uint64 titleId)
		{
			return std::find(std::begin(kTitlesNeedingLessMem2), std::end(kTitlesNeedingLessMem2), titleId) != std::end(kTitlesNeedingLessMem2);
		}
	}

	void DCInvalidateRange(MPTR addr, uint32 size)
	{
	}

	void DCFlushRange(MPTR addr, uint32 size)
	{
		NotifyLinesWritten(addr, size);
	}

	void DCFlushRangeNoSync(MPTR addr, uint32 size)
	{
		NotifyLinesWritten(addr, size);
	}

	void DCStoreRange(MPTR addr, uint32 size)
	{
	}

	void DCStoreRangeNoSync(MPTR addr, uint32 size)
	{
		NotifyLinesWritten(addr, size);
	}

	// zeroes every cache line touched by the range
	void DCZeroRange(MPTR addr, uint32 size)
	{
		MPTR start = addr & ~(kCacheLine - 1);
		uint32 lines = ((addr & (kCacheLine - 1)) + size + kCacheLine - 1) / kCacheLine;
		if (lines == 0)
			return;
		memset(memory_getPointerFromVirtualOffset(start), 0, lines * kCacheLine);
		LatteBufferCache_notifyDCFlush(start, lines * kCacheLine);
	}

	// only the 0xE8000000-0xEBFFFFFF window is valid for these checks
	bool OSIsAddressRangeDCValid(uint32 startOffset, uint32 range)
	{
		constexpr uint32 kLow = 0xE8000000;
		constexpr uint32 kHigh = 0xEC000000;
		uint32 last = startOffset + range - 1;
		return startOffset >= kLow && startOffset < kHigh && last >= kLow && last < kHigh;
	}

	void* coreinit_memset(void* dst, uint32 value, uint32 size)
	{
		memset(dst, value, size);
		return dst;
	}

	void* coreinit_memcpy(MEMPTR<void> dst, MEMPTR<void> src, uint32 size)
	{
		// The Swapper copies to 0xFFFFFFFF; how the console wraps such a copy is unknown
		cemu_assert_debug(dst.GetMPTR() != 0xFFFFFFFF);
		if (size != 0)
		{
			memcpy(dst.GetPtr(), src.GetPtr(), size);
			LatteBufferCache_notifyDCFlush(dst.GetMPTR(), size); // the console's memcpy flushes too
		}
		return dst.GetPtr();
	}

	void* coreinit_memmove(MEMPTR<void> dst, void* src, uint32 size)
	{
		if (size != 0)
		{
			memmove(dst.GetPtr(), src, size);
			LatteBufferCache_notifyDCFlush(dst.GetMPTR(), size);
		}
		return dst.GetPtr();
	}

	void* OSBlockMove(MEMPTR<void> dst, MEMPTR<void> src, uint32 size, bool flushDC)
	{
		if (size != 0)
		{
			memmove(dst.GetPtr(), src.GetPtr(), size);
			if (flushDC)
				LatteBufferCache_notifyDCFlush(dst.GetMPTR(), size);
		}
		return dst.GetPtr();
	}

	void* OSBlockSet(MEMPTR<void> dst, uint32 value, uint32 size)
	{
		memset(dst.GetPtr(), value & 0xFF, size);
		return dst.GetPtr();
	}

	MPTR OSEffectiveToPhysical(MPTR effectiveAddr)
	{
		return memory_virtualToPhysical(effectiveAddr);
	}

	void OSMemoryBarrier()
	{
		// guest memory accesses are already ordered for the title
	}

	// For reference, a console browser reports MEM1 at 0xF4000000 (32MB) and MEM2 at 0x106DE000
	void OSGetMemBound(sint32 memType, MEMPTR<void>* offsetOutput, uint32be* sizeOutput)
	{
		MPTR base = MPTR_NULL;
		uint32 size = 0;
		if (memType == 1)
		{
			base = mmuRange_MEM1.getBase();
			size = mmuRange_MEM1.getSize();
		}
		else if (memType == 2)
		{
			// MEM2 available to the title starts where the library data allocator currently is
			base = RPLLoader_GetDataAllocatorAddr();
			if (NeedsLessMem2(CafeSystem::GetForegroundTitleId()))
			{
				cemuLog_logDebug(LogType::Force, "OSGetMemBound: reporting 48MB less MEM2 for this title");
				base += kMem2Reduction;
			}
			size = mmuRange_MEM2.getEnd() - base;
		}
		else
		{
			cemu_assert_debug(false);
		}
		if (offsetOutput)
			*offsetOutput = base;
		if (sizeOutput)
			*sizeOutput = size;
	}

	void InitializeMemory()
	{
		cafeExportRegister("coreinit", DCInvalidateRange, LogType::Placeholder);
		cafeExportRegister("coreinit", DCFlushRange, LogType::Placeholder);
		cafeExportRegister("coreinit", DCFlushRangeNoSync, LogType::Placeholder);
		cafeExportRegister("coreinit", DCStoreRange, LogType::Placeholder);
		cafeExportRegister("coreinit", DCStoreRangeNoSync, LogType::Placeholder);
		cafeExportRegister("coreinit", DCZeroRange, LogType::Placeholder);
		cafeExportRegister("coreinit", OSIsAddressRangeDCValid, LogType::Placeholder);

		cafeExportRegisterFunc(coreinit_memcpy, "coreinit", "memcpy", LogType::Placeholder);
		cafeExportRegisterFunc(coreinit_memset, "coreinit", "memset", LogType::Placeholder);
		cafeExportRegisterFunc(coreinit_memmove, "coreinit", "memmove", LogType::Placeholder);
		cafeExportRegister("coreinit", OSBlockMove, LogType::Placeholder);
		cafeExportRegister("coreinit", OSBlockSet, LogType::Placeholder);

		cafeExportRegister("coreinit", OSEffectiveToPhysical, LogType::Placeholder);
		cafeExportRegister("coreinit", OSMemoryBarrier, LogType::Placeholder);

		cafeExportRegister("coreinit", OSGetMemBound, LogType::Placeholder);
	}
}
