// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "util/MemMapper/MemMapper.h"

// Title controlled virtual memory: a title reserves a virtual range with OSAllocVirtAddr and then
// backs it with memory using OSMapMemory. Reservations come from a fixed window and are handed
// out first fit, in page granularity.
namespace coreinit
{
	namespace
	{
		constexpr uint32 kMapReadOnly = 1;
		constexpr uint32 kMapReadWrite = 2;

		// reservable virtual window (would be per process on the console)
		constexpr uint32 kVirtWindowStart = 0xA0000000;
		constexpr uint32 kVirtWindowSize = 0x40000000;
		// physical range reported to titles
		constexpr uint32 kPhysRangeStart = 0x10000000;
		constexpr uint32 kPhysRangeSize = 0x40000000;

		struct Reservation
		{
			MPTR start;
			uint32 size; // page rounded
			uint32 alignment;
		};

		std::mutex s_reservationsLock;
		std::vector<Reservation> s_reservations; // in order of creation

		bool InVirtWindow(MPTR addr)
		{
			return addr >= kVirtWindowStart && addr < kVirtWindowStart + kVirtWindowSize;
		}

		uint32 RoundUp(uint32 value, uint32 granularity)
		{
			return (value + granularity - 1) & ~(granularity - 1);
		}

		// first fit search: on a collision continue right behind the colliding reservation
		MPTR ReserveRange(uint32 size, uint32 alignment)
		{
			std::lock_guard lock(s_reservationsLock);
			uint32 page = std::max<uint32>((uint32)MemMapper::GetPageSize(), Espresso::MEM_PAGE_SIZE);
			uint32 candidate = kVirtWindowStart;
			for (;;)
			{
				candidate = RoundUp(RoundUp(candidate, alignment), page);
				uint32 candidateEnd = RoundUp(candidate + size, page);
				if (candidateEnd > kVirtWindowStart + kVirtWindowSize)
				{
					cemuLog_log(LogType::APIErrors, "OSAllocVirtAddr: no free range of {:#x} bytes left", size);
					return MPTR_NULL;
				}
				const Reservation* collision = nullptr;
				for (const Reservation& r : s_reservations)
				{
					if (candidate < r.start + r.size && candidateEnd > r.start)
					{
						collision = &r;
						break;
					}
				}
				if (!collision)
				{
					s_reservations.push_back({ candidate, candidateEnd - candidate, alignment });
					return candidate;
				}
				candidate = collision->start + collision->size;
			}
		}

		bool ReleaseRange(MPTR start)
		{
			std::lock_guard lock(s_reservationsLock);
			auto it = std::find_if(s_reservations.begin(), s_reservations.end(), [&](const Reservation& r) { return r.start == start; });
			if (it == s_reservations.end())
				return false;
			s_reservations.erase(it);
			return true;
		}
	}

	void OSGetAvailPhysAddrRange(uint32be* physRangeStart, uint32be* physRangeSize)
	{
		*physRangeStart = kPhysRangeStart;
		*physRangeSize = kPhysRangeSize;
	}

	void* OSAllocVirtAddr(MEMPTR<void> address, uint32 size, uint32 align)
	{
		if (align == 0)
			align = 1;
		if (align != 1)
			assert_dbg(); // only unaligned reservations are supported
		cemu_assert_debug(address == nullptr); // reserving at a fixed address is not supported
		return MEMPTR<void>(ReserveRange(size, align));
	}

	uint32 OSFreeVirtAddr(MEMPTR<void> address)
	{
		if (ReleaseRange(address.GetMPTR()))
			return 1;
		cemuLog_log(LogType::APIErrors, "OSFreeVirtAddr: {:08x} is not a reserved range", address.GetMPTR());
		return 0;
	}

	uint32 OSMapMemory(MPTR virtualAddress, MPTR physicalAddress, uint32 size, uint32 mode)
	{
		if (!InVirtWindow(virtualAddress))
		{
			cemuLog_log(LogType::APIErrors, "OSMapMemory: {:08x} is outside the mappable range", virtualAddress);
			return 0;
		}
		MemMapper::PAGE_PERMISSION permission = MemMapper::PAGE_PERMISSION::P_NONE;
		if (mode == kMapReadOnly)
			permission = MemMapper::PAGE_PERMISSION::P_READ;
		else if (mode == kMapReadWrite)
			permission = MemMapper::PAGE_PERMISSION::P_RW;
		else
			cemu_assert_unimplemented();
		// the physical address is ignored, every mapping gets fresh memory
		if (!MemMapper::AllocateMemory(memory_getPointerFromVirtualOffset(virtualAddress), size, permission, true))
		{
			cemuLog_log(LogType::Force, "OSMapMemory: could not map {:#x} bytes at {:08x}", size, virtualAddress);
			return 0;
		}
		return 1;
	}

	uint32 OSUnmapMemory(MPTR virtualAddress, uint32 size)
	{
		if (!InVirtWindow(virtualAddress))
			cemu_assert_suspicious();
		cemu_assert((size % MemMapper::GetPageSize()) == 0);
		MemMapper::FreeMemory(memory_getPointerFromVirtualOffset(virtualAddress), size, true);
		return 1;
	}

	void InitializeMemoryMapping()
	{
		s_reservations.clear();
		cafeExportRegister("coreinit", OSGetAvailPhysAddrRange, LogType::CoreinitMemoryMapping);
		cafeExportRegister("coreinit", OSAllocVirtAddr, LogType::CoreinitMemoryMapping);
		cafeExportRegister("coreinit", OSFreeVirtAddr, LogType::CoreinitMemoryMapping);
		cafeExportRegister("coreinit", OSMapMemory, LogType::CoreinitMemoryMapping);
		cafeExportRegister("coreinit", OSUnmapMemory, LogType::CoreinitMemoryMapping);
	}
}
