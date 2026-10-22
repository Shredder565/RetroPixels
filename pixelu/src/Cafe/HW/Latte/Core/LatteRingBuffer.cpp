// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteRingBuffer.h"

LatteRingBuffer_t* LatteRingBuffer_create(uint8* data, uint32 size)
{
	auto* rb = (LatteRingBuffer_t*)malloc(sizeof(LatteRingBuffer_t));
	*rb = { data, (sint32)size, 0 };
	return rb;
}

// alignment must be a power of two; an allocation that would reach the end starts over at 0
uint8* LatteRingBuffer_allocate(LatteRingBuffer_t* rb, sint32 size, sint32 alignment)
{
#ifdef CEMU_DEBUG_ASSERT
	cemu_assert_debug(size < rb->size);
#endif
	sint32 start = (rb->writeIndex + alignment - 1) & ~(alignment - 1);
	if (start + size >= rb->size)
		start = 0;
	rb->writeIndex = start + size;
	return rb->data + start;
}
