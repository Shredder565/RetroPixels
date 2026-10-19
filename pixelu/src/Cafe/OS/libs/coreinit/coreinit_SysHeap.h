// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

namespace coreinit
{
	// creates the system heap; called once per boot before the application starts
	void InitSysHeap();

	void* OSAllocFromSystem(uint32 size, uint32 alignment);
	void OSFreeToSystem(void* ptr);

	void InitializeSysHeap();
}
