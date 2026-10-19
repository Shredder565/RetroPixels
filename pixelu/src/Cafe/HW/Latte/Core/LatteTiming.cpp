// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VsyncDriver.h"
#endif
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"

// Emulated display timing. The GPU thread polls LatteTiming_HandleTimedVsync; each
// elapsed refresh period raises the GX2 VSYNC event and, every swapInterval periods,
// completes one pending scan buffer flip. With host driven vsync the refreshes come
// from the swapchain's vsync thread instead and the timer only drives housekeeping.

namespace
{
	sint32 s_overrideHz = -1; // graphic pack override, <= 0 = default
	bool s_hostDriven = false;
	HRTick s_hostVsyncTick = 0; // emulated vsync time last reached from host vsyncs
	uint32 s_refreshesSinceFlip = 0;

	// one refresh period in timer ticks. Without an override the rate is
	// 60 Hz * 1000/1002 (59.88 Hz), just under the console's 59.94 Hz.
	HRTick RefreshPeriod()
	{
		HRTick freq = HighResolutionTimer::getFrequency();
		if (s_overrideHz > 0)
			return freq / (uint64)s_overrideHz;
		return freq * 1000ull / 1002ull / 60ull;
	}

	// Titles whose flips are counted against GX2's own request counter instead of
	// the GPU side request count (keeps them from running ahead of SwapScanBuffers)
	bool UsesSharedAreaFlipThrottle(uint64 titleId)
	{
		return titleId == 0x00050000101c9500 || titleId == 0x00050000101c9400 || titleId == 0x0005000e101c9300;
	}

	void IncrementFlipExecuteCount(gx2GPUSharedArea_t* area)
	{
		area->flipExecuteCountBE = _swapEndianU32(_swapEndianU32(area->flipExecuteCountBE) + 1);
	}

	// one emulated display refresh
	void Refresh()
	{
		if (!LatteGPUState.gx2InitCalled)
			return;
		gx2GPUSharedArea_t* area = LatteGPUState.sharedArea;
		s_refreshesSinceFlip++;
		const uint32 interval = area ? area->swapInterval : 1;
		if (s_refreshesSinceFlip >= interval)
		{
			if (area)
			{
				if (UsesSharedAreaFlipThrottle(CafeSystem::GetForegroundTitleId()))
				{
					const uint32 requested = _swapEndianU32(area->flipRequestCountBE);
					const uint32 executed = _swapEndianU32(area->flipExecuteCountBE);
					// allow executing up to 3 flips ahead of the requests
					if (requested >= executed || executed - requested < 4)
						IncrementFlipExecuteCount(area);
					LatteGPUState.flipCounter++;
				}
				else if (LatteGPUState.flipRequestCount > 0)
				{
					LatteGPUState.flipRequestCount.fetch_sub(1);
					IncrementFlipExecuteCount(area);
				}
			}
			GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::FLIP);
			s_refreshesSinceFlip = 0;
		}
		GX2::__GX2NotifyEvent(GX2::GX2CallbackEventType::VSYNC);
	}

	// called from the host vsync thread
	void OnHostVSync()
	{
		if (!s_hostDriven)
			return;
		const HRTick now = HighResolutionTimer::now().getTick();
		const HRTick period = RefreshPeriod();
		const HRTick behind = now - s_hostVsyncTick;
		// host displays faster than the emulated rate skip refreshes
		if (behind < period)
			return;
		// far behind (e.g. after a stall): resynchronise instead of catching up
		if (behind / period >= 10)
			s_hostVsyncTick = now;
		else
			s_hostVsyncTick += period;
		Refresh();
	}
}

void LatteTiming_setCustomVsyncFrequency(sint32 frequency)
{
	s_overrideHz = frequency;
}

void LatteTiming_disableCustomVsyncFrequency()
{
	s_overrideHz = -1;
}

bool LatteTiming_getCustomVsyncFrequency(sint32& customFrequency)
{
	const sint32 hz = s_overrideHz;
	if (hz <= 0)
		return false;
	customFrequency = hz;
	return true;
}

void LatteTiming_EnableHostDrivenVSync()
{
	if (s_hostDriven)
		return;
#ifdef ENABLE_VULKAN
	VsyncDriver_startThread(OnHostVSync);
	s_hostDriven = true;
#endif
}

void LatteTiming_Init()
{
	LatteGPUState.timer_frequency = HighResolutionTimer::getFrequency();
	LatteGPUState.timer_bootUp = HighResolutionTimer::now().getTick();
	LatteGPUState.timer_nextVSync = LatteGPUState.timer_bootUp + RefreshPeriod();
}

void LatteTiming_HandleTimedVsync()
{
	const uint64 now = HighResolutionTimer::now().getTick();
	if (now < LatteGPUState.timer_nextVSync)
		return;
	if (!s_hostDriven)
		Refresh();
	// the timer also paces query and readback completion when vsync comes from the host
	LatteQuery_UpdateFinishedQueries();
	LatteTextureReadback_UpdateFinishedTransfers(false);
	const uint64 period = RefreshPeriod();
	const uint64 late = (now - LatteGPUState.timer_nextVSync) / period;
	// drop refreshes that were missed by two or more periods
	LatteGPUState.timer_nextVSync += late >= 2 ? period * (late + 1) : period;
}
