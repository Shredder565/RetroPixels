// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Thin layer over the host's virtual memory API. All functions return
// nullptr on failure.
namespace MemMapper
{
	enum class PAGE_PERMISSION : uint32
	{
		P_READ = 0x01,
		P_WRITE = 0x02,
		P_EXECUTE = 0x04,
		P_NONE = 0,
		P_RW = P_READ | P_WRITE,
		P_RWX = P_READ | P_WRITE | P_EXECUTE
	};
	DEFINE_ENUM_FLAG_OPERATORS(PAGE_PERMISSION);

	size_t GetPageSize();

	// reserves address space without backing memory (inaccessible until allocated)
	void* ReserveMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags);
	void FreeReservation(void* baseAddr, size_t size);

	// commits memory, either inside an earlier reservation or as a new mapping
	void* AllocateMemory(void* baseAddr, size_t size, PAGE_PERMISSION permissionFlags, bool fromReservation = false);
	void FreeMemory(void* baseAddr, size_t size, bool fromReservation = false);
};
