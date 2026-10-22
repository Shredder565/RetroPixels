// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

namespace coreinit
{
	// ring of block pointers holding the free blocks of an IPC buffer pool
	struct IPCBufPoolFreeRing
	{
		/* +0x00 */ uint32be putPos;
		/* +0x04 */ uint32be takePos;   // -1 while the ring is empty
		/* +0x08 */ uint32be used;
		/* +0x0C */ uint32be capacity;
		/* +0x10 */ MEMPTR<MEMPTR<uint8>> slots;
	};
	static_assert(sizeof(IPCBufPoolFreeRing) == 0x14);

	// fixed size block allocator for IPC request buffers, built inside a caller supplied area:
	// [pool header][free ring slots][block 0][block 1]...
	struct IPCBufPool_t
	{
		/* +0x00 */ uint32be magic;
		/* +0x04 */ MEMPTR<void> area;
		/* +0x08 */ uint32be areaSize;
		/* +0x0C */ uint32be createFlags;   // last argument of IPCBufPoolCreate, kept as is
		/* +0x10 */ uint32be reserved10;
		/* +0x14 */ uint32be blockSize;     // requested size rounded up to 0x40
		/* +0x18 */ uint32be blockStride;   // same as blockSize
		/* +0x1C */ uint32be blockCount;
		/* +0x20 */ MEMPTR<uint8> firstBlock;
		/* +0x24 */ uint32be slotBytes;     // size of the free ring slot array
		/* +0x28 */ IPCBufPoolFreeRing freeRing;
		/* +0x3C */ coreinit::OSMutex mutex;
		/* +0x68 */ uint32 reserved68;
	};
	static_assert(sizeof(IPCBufPool_t) == 0x6C);

	uint8* IPCBufPoolAllocate(IPCBufPool_t* ipcBufPool, uint32 size);
	IPCBufPool_t* IPCBufPoolCreate(uint8* bufferArea, uint32 bufferSize, uint32 entrySize, uint32be* entryCountOutput, uint32 uknR7);
	sint32 IPCBufPoolFree(IPCBufPool_t* ipcBufPool, uint8* entry);

	void InitializeIPCBuf();
} // namespace coreinit
