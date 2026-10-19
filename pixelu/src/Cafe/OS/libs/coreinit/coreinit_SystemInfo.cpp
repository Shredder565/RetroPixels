// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_SystemInfo.h"

namespace coreinit
{
	namespace
	{
		SysAllocator<OSSystemInfo> s_systemInfo;
	}

	const OSSystemInfo& OSGetSystemInfo()
	{
		return *s_systemInfo.GetPtr();
	}

	void InitializeSystemInfo()
	{
		cemu_assert(ppcCyclesSince2000 != 0); // the boot time must be known by now
		OSSystemInfo& info = *s_systemInfo.GetPtr();
		info.busClock = ESPRESSO_BUS_CLOCK;
		info.coreClock = ESPRESSO_CORE_CLOCK;
		info.ticksSince2000 = ESPRESSO_CORE_CLOCK_TO_TIMER_CLOCK(ppcCyclesSince2000);
		// core 1 has the large L2. The value reported for it has always been 2 * 1024 * 1924
		// (not quite 2MB); kept so titles see the same number as before
		info.l2cacheSize[0] = 512 * 1024;
		info.l2cacheSize[1] = 2 * 1024 * 1924;
		info.l2cacheSize[2] = 512 * 1024;
		info.coreClockToBusClockRatio = 5;
		cafeExportRegister("coreinit", OSGetSystemInfo, LogType::Placeholder);
	}
}
