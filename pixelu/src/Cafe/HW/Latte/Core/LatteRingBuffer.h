// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// linear allocator over a fixed buffer that restarts at the beginning when full
typedef struct
{
	uint8* data;
	sint32 size;
	sint32 writeIndex;
}LatteRingBuffer_t;

LatteRingBuffer_t* LatteRingBuffer_create(uint8* data, uint32 size);
uint8* LatteRingBuffer_allocate(LatteRingBuffer_t* rb, sint32 size, sint32 alignment);
