// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

namespace coreinit
{
	void InitializeMemory();

	// memType 1 = MEM1, 2 = MEM2 (the part not used by the system and libraries)
	void OSGetMemBound(sint32 memType, MEMPTR<void>* offsetOutput, uint32be* sizeOutput);

	void* OSBlockMove(MEMPTR<void> dst, MEMPTR<void> src, uint32 size, bool flushDC);
	void* OSBlockSet(MEMPTR<void> dst, uint32 value, uint32 size);

	void OSMemoryBarrier();
}
