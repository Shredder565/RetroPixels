// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Address-space allocator for a buffer of virtualSize bytes. Allocations are
// rounded to 256 bytes and placed at the lowest free 256-byte aligned offset.

// one allocation, [startOffset, endOffset); entries are kept sorted by offset
struct VirtualBufferHeapEntry_t
{
	uint32 startOffset;
	uint32 endOffset;
	VirtualBufferHeapEntry_t* next;
	VirtualBufferHeapEntry_t* previous;
};

struct VirtualBufferHeap_t
{
	uint32 virtualSize;
	void* baseAddress; // added to offsets by the *Addr functions
	VirtualBufferHeapEntry_t* firstEntry; // lowest allocation
	VirtualBufferHeapEntry_t* firstUnusedEntry; // recycled entry records
	uint32 updateTrackIndex;
	struct
	{
		uint32 numActiveAllocs;
		uint32 allocatedMemory;
	}stats;
};

VirtualBufferHeap_t* virtualBufferHeap_create(uint32 virtualHeapSize, void* baseAddr = nullptr);
// nullptr if there is no free space for size bytes
VirtualBufferHeapEntry_t* virtualBufferHeap_allocate(VirtualBufferHeap_t* bufferHeap, uint32 size);
void virtualBufferHeap_free(VirtualBufferHeap_t* bufferHeap, VirtualBufferHeapEntry_t* entry);

// same as above, but in terms of baseAddress + offset
void* virtualBufferHeap_allocateAddr(VirtualBufferHeap_t* bufferHeap, uint32 size);
void virtualBufferHeap_freeAddr(VirtualBufferHeap_t* bufferHeap, void* addr);
