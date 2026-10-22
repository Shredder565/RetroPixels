// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Espresso/Const.h"

namespace coreinit
{
	// broken-down date as returned by OSTicksToCalendarTime
	typedef struct
	{
		/* +0x00 */ sint32be second;      // 0-59
		/* +0x04 */ sint32be minute;      // 0-59
		/* +0x08 */ sint32be hour;        // 0-23
		/* +0x0C */ sint32be dayOfMonth;  // 1-31
		/* +0x10 */ sint32be month;       // 0-11
		/* +0x14 */ sint32be year;        // 2000-...
		/* +0x18 */ sint32be dayOfWeek;   // 0-6, 0 = Sunday
		/* +0x1C */ sint32be dayOfYear;   // 0-365
		/* +0x20 */ sint32be millisecond; // 0-999
		/* +0x24 */ sint32be microsecond; // 0-999
	}OSCalendarTime_t;
	static_assert(sizeof(OSCalendarTime_t) == 0x28);

	// clocks of the Espresso CPU; OS time is counted in timer ticks (bus clock / 4)
	namespace EspressoTime
	{
		typedef sint64 TimerTicks;

		constexpr sint64 GetTimerClock() { return Espresso::TIMER_CLOCK; }
		constexpr sint64 GetBusClock() { return Espresso::BUS_CLOCK; }
		constexpr sint64 GetCoreClock() { return Espresso::CORE_CLOCK; }

		// the timer runs at 62.15625 MHz, which is exactly 1989/32000 ticks per nanosecond
		inline TimerTicks ConvertNsToTimerTicks(uint64 ns)
		{
			constexpr uint64 TICKS_PER_32000NS = (uint64)GetTimerClock() / 31250ull;
			static_assert(TICKS_PER_32000NS * 31250ull == (uint64)GetTimerClock());
			return static_cast<TimerTicks>(ns * TICKS_PER_32000NS / 32000ull);
		}

		inline TimerTicks ConvertMsToTimerTicks(uint64 ms)
		{
			return static_cast<TimerTicks>(ms * (uint64)GetTimerClock() / 1000ull);
		}
	};

	void OSTicksToCalendarTime(uint64 ticks, OSCalendarTime_t* calenderStruct);

	uint64 OSGetSystemTime(); // timer ticks since power on
	uint64 OSGetTime();       // timer ticks since 2000-01-01 00:00
	uint32 OSGetSystemTick();
	uint32 OSGetTick();

	void InitializeTimeAndCalendar();
};
