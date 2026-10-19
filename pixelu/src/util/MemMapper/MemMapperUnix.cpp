// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/MemMapper/MemMapper.h"

#include <unistd.h>
#include <sys/mman.h>

namespace MemMapper
{
	namespace
	{
		int toProt(PAGE_PERMISSION perm)
		{
			const bool r = HAS_FLAG(perm, PAGE_PERMISSION::P_READ);
			const bool w = HAS_FLAG(perm, PAGE_PERMISSION::P_WRITE);
			const bool x = HAS_FLAG(perm, PAGE_PERMISSION::P_EXECUTE);
			if (r && w && x)
				return PROT_READ | PROT_WRITE | PROT_EXEC;
			if (r && w)
				return PROT_READ | PROT_WRITE;
			if (r && !x)
				return PROT_READ;
			cemu_assert_unimplemented(); // only R, RW and RWX are used
			return PROT_NONE;
		}

		void* mapAnonymous(void* addr, size_t size, int prot)
		{
			void* p = mmap(addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			return p == MAP_FAILED ? nullptr : p;
		}
	}

	size_t GetPageSize()
	{
		static const size_t s_pageSize = (size_t)sysconf(_SC_PAGESIZE);
		return s_pageSize;
	}

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		(void)permissionFlags; // reserved pages are inaccessible until AllocateMemory
		return mapAnonymous(baseAddr, size, PROT_NONE);
	}

	void FreeReservation(void* baseAddr, size_t size)
	{
		munmap(baseAddr, size);
	}

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation)
	{
		if (!fromReservation)
			return mapAnonymous(baseAddr, size, toProt(permissionFlags));
		// mprotect wants a page-aligned start
		const uintptr_t pageMask = (uintptr_t)GetPageSize() - 1;
		void* pageStart = (void*)((uintptr_t)baseAddr & ~pageMask);
		if (mprotect(pageStart, size, toProt(permissionFlags)) != 0)
			return nullptr;
		return baseAddr;
	}

	void FreeMemory(void* baseAddr, size_t size, bool fromReservation)
	{
		if (fromReservation)
			mprotect(baseAddr, size, PROT_NONE); // back to reserved-only
		else
			munmap(baseAddr, size);
	}
};
