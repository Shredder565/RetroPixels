// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/libs/coreinit/coreinit.h"

namespace coreinit
{
	// returned by OSGetSystemInfo, lives in guest memory
	struct OSSystemInfo
	{
		/* +0x00 */ uint32be busClock;
		/* +0x04 */ uint32be coreClock;
		/* +0x08 */ uint64be ticksSince2000;			  // timer ticks at boot
		/* +0x10 */ uint32be l2cacheSize[PPC_CORE_COUNT];
		/* +0x1C */ uint32be coreClockToBusClockRatio;
	};
	static_assert(sizeof(OSSystemInfo) == 0x20);

	const OSSystemInfo& OSGetSystemInfo();

	void InitializeSystemInfo();
}
