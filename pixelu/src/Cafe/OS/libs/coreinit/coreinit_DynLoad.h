// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// OSDynLoad: runtime loading of RPL modules, export lookup and the allocator
// that the loader uses for guest-side module memory.
namespace coreinit
{
	// Module description handed to notify callbacks and returned by OSDynLoad_GetRPLInfo.
	struct OSDynLoad_NotifyData
	{
		/* +0x00 */ MEMPTR<char> name;

		/* +0x04 */ uint32be textAddr;
		/* +0x08 */ uint32be textOffset;
		/* +0x0C */ uint32be textSize;

		/* +0x10 */ uint32be dataAddr;
		/* +0x14 */ uint32be dataOffset;
		/* +0x18 */ uint32be dataSize;

		/* +0x1C */ uint32be readAddr;
		/* +0x20 */ uint32be readOffset;
		/* +0x24 */ uint32be readSize;
	};
	static_assert(sizeof(OSDynLoad_NotifyData) == 0x28);

	// guest callback: (module handle ptr, user context, reason 1=loaded/2=unloaded, info)
	using OSDynLoadNotifyFunc = void (*)(uint32be* module, void* userContext, sint32 notifyReason, OSDynLoad_NotifyData* infos);

	uint32 OSDynLoad_SetAllocator(MPTR allocFunc, MPTR freeFunc);
	void OSDynLoad_SetTLSAllocator(MPTR allocFunc, MPTR freeFunc);
	uint32 OSDynLoad_GetAllocator(betype<MPTR>* funcAlloc, betype<MPTR>* funcFree);
	void OSDynLoad_GetTLSAllocator(betype<MPTR>* funcAlloc, betype<MPTR>* funcFree);

	// allocate/free through the guest allocator set with OSDynLoad_SetAllocator
	void* OSDynLoad_AllocatorAlloc(sint32 size, sint32 alignment);
	void OSDynLoad_AllocatorFree(void* mem);

	uint32 OSDynLoad_Acquire(const char* libName, uint32be* moduleHandleOut);
	void OSDynLoad_Release(uint32 moduleHandle);
	uint32 OSDynLoad_FindExport(uint32 moduleHandle, uint32 isData, const char* exportName, betype<MPTR>* addrOut);

	uint32 OSDynLoad_GetModuleName(uint32 moduleHandle, char* nameBuf, sint32be* nameBufSize);
	sint32 OSDynLoad_GetNumberOfRPLs();
	uint32 OSDynLoad_GetRPLInfo(uint32 first, uint32 count, OSDynLoad_NotifyData* outInfos);

	uint32 OSDynLoad_AddNotifyCallback(MEMPTR<OSDynLoadNotifyFunc> notifyFn, MEMPTR<void> userContext);
	uint32 OSDynLoad_DelNotifyCallback(MEMPTR<OSDynLoadNotifyFunc> notifyFn, MEMPTR<void> userContext);

	void InitializeDynLoad();
}
