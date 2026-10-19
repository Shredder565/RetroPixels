// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/MemMapper/MemMapper.h"

#include <Windows.h>

namespace MemMapper
{
	namespace
	{
		DWORD toProtect(PAGE_PERMISSION perm)
		{
			const bool r = HAS_FLAG(perm, PAGE_PERMISSION::P_READ);
			const bool w = HAS_FLAG(perm, PAGE_PERMISSION::P_WRITE);
			const bool x = HAS_FLAG(perm, PAGE_PERMISSION::P_EXECUTE);
			if (r && w && x)
				return PAGE_EXECUTE_READWRITE;
			if (r && w)
				return PAGE_READWRITE;
			if (r && !x)
				return PAGE_READONLY;
			cemu_assert_unimplemented(); // only R, RW and RWX are used
			return PAGE_NOACCESS;
		}
	}

	size_t GetPageSize()
	{
		static const size_t s_pageSize = []() {
			SYSTEM_INFO info;
			GetSystemInfo(&info);
			return (size_t)info.dwPageSize;
		}();
		return s_pageSize;
	}

	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags)
	{
		return VirtualAlloc(baseAddr, size, MEM_RESERVE, toProtect(permissionFlags));
	}

	void FreeReservation(void* baseAddr, size_t size)
	{
		(void)size; // MEM_RELEASE always frees the whole reservation and requires size 0
		VirtualFree(baseAddr, 0, MEM_RELEASE);
	}

	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation)
	{
		const DWORD type = fromReservation ? MEM_COMMIT : (MEM_RESERVE | MEM_COMMIT);
		return VirtualAlloc(baseAddr, size, type, toProtect(permissionFlags));
	}

	void FreeMemory(void* baseAddr, size_t size, bool fromReservation)
	{
		if (fromReservation)
			VirtualFree(baseAddr, size, MEM_DECOMMIT);
		else
			VirtualFree(baseAddr, 0, MEM_RELEASE);
	}
};
