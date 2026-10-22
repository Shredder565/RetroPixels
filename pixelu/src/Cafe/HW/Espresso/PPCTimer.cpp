// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Guest clock derived from the host time stamp counter. The TSC rate is
// measured once at start-up; afterwards every query converts the TSC delta
// since the previous query into Espresso core cycles, carrying the division
// remainder so no time is lost between calls.

#include "Cafe/HW/Espresso/Const.h"
#include "config/ActiveSettings.h"
#include "util/helpers/fspinlock.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "Common/cpu_features.h"

#if defined(ARCH_X86_64)
#include <immintrin.h>
#endif

namespace
{
	// host TSC ticks per second, 0 until the measurement finished
	std::atomic<uint64> s_tscFrequency{ 0 };

	// state of the converter, protected by s_clockLock
	FSpinlock s_clockLock;
	uint64 s_lastTsc = 0;          // TSC value of the previous query
	uint64 s_carryLow = 0;         // remainder of tsc * CORE_CLOCK / frequency
	uint64 s_carryHigh = 0;
	uint64 s_guestTicks = 0;       // returned clock value

	// floor(a * b / d) without intermediate overflow (result must fit in 64 bits)
	uint64 MulDiv(uint64 a, uint64 b, uint64 d)
	{
		uint64 high;
		uint64 low = _umul128(a, b, &high);
		uint64 remainder;
		return _udiv128(high, low, d, &remainder);
	}

	// compares the TSC against the high resolution timer over ~3 seconds
	uint64 MeasureTscFrequency()
	{
#if defined(ARCH_X86_64)
		if (!g_CPUFeatures.x86.invariant_tsc)
			cemuLog_log(LogType::Force, "Invariant TSC not supported");
#endif
		_mm_mfence();
		const uint64 tscBegin = __rdtsc();
		const HRTick hrBegin = HighResolutionTimer::now().getTick();
		const auto wallBegin = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - wallBegin < std::chrono::seconds(3))
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		_mm_mfence();
		const HRTick hrEnd = HighResolutionTimer::now().getTick();
		const uint64 tscEnd = __rdtsc();

		uint64 hrFrequency = 0;
		const uint64 hrElapsed = HighResolutionTimer::getTimeDiffEx(hrBegin, hrEnd, hrFrequency);
		return MulDiv(tscEnd - tscBegin, hrFrequency, hrElapsed);
	}
}

void PPCTimer_init()
{
	std::thread([]() { s_tscFrequency = MeasureTscFrequency(); }).detach();
	s_lastTsc = __rdtsc();
}

void PPCTimer_waitForInit()
{
	while (s_tscFrequency.load() == 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void PPCTimer_start()
{
	s_lastTsc = __rdtsc();
	s_guestTicks = 0;
}

uint64 PPCTimer_getRawTsc()
{
	return __rdtsc();
}

uint64 PPCTimer_microsecondsToTsc(uint64 us)
{
	return (us * s_tscFrequency.load()) / 1000000ULL;
}

uint64 PPCTimer_tscToMicroseconds(uint64 us)
{
	return MulDiv(us, 1000000ULL, s_tscFrequency.load());
}

// thread safe, monotonic
uint64 PPCTimer_getFromRDTSC()
{
	s_clockLock.lock();
	_mm_mfence();
	const uint64 now = __rdtsc();
	// a TSC that went backwards (core migration) counts as no time passed
	uint64 delta = 0;
	if ((sint64)(now - s_lastTsc) > 0)
		delta = now - s_lastTsc;
	if (now > s_lastTsc)
		s_lastTsc = now;

	// carry += delta * CORE_CLOCK (128 bit)
	uint64 productHigh;
	uint64 productLow = _umul128(delta, Espresso::CORE_CLOCK, &productHigh);
	uint64 sumLow = s_carryLow + productLow;
	s_carryHigh += productHigh + (sumLow < productLow ? 1 : 0);
	s_carryLow = sumLow;

	// whole core cycles elapsed, keep the remainder for the next query
	uint64 remainder;
	uint64 cycles = _udiv128(s_carryHigh, s_carryLow, s_tscFrequency.load(), &remainder);
	s_carryLow = remainder;
	s_carryHigh = 0;

	// the guest clock runs at 8x the core clock divided by the user's
	// timer shift setting
	cycles <<= 3;
	cycles >>= ActiveSettings::GetTimerShiftFactor();

	s_guestTicks += cycles;
	const uint64 result = s_guestTicks;
	s_clockLock.unlock();
	return result;
}
