// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"

/* OS time
 * The time base register counts timer ticks (core clock / 20). OSGetTime adds the offset of the
 * emulated real time clock so that tick 0 is 2000-01-01 00:00:00 (a Saturday). Calendar
 * conversion uses the proleptic Gregorian calendar with days counted from 0000-01-01. */

namespace coreinit
{
	namespace
	{
		constexpr uint64 TICKS_PER_SECOND = ESPRESSO_TIMER_CLOCK;
		constexpr uint64 SECONDS_PER_DAY = 24 * 60 * 60;

		// first day of each month for common and leap years
		constexpr uint32 MONTH_START[2][12] =
		{
			{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 },
			{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 },
		};

		constexpr bool IsLeap(uint32 year)
		{
			return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
		}

		// number of leap years in [0, year) (year 0 is a leap year)
		constexpr uint32 LeapYearsBefore(uint32 year)
		{
			if (year == 0)
				return 0;
			return (year + 3) / 4 - (year - 1) / 100 + (year - 1) / 400;
		}

		constexpr uint64 DaysBeforeYear(uint32 year)
		{
			return (uint64)year * 365 + LeapYearsBefore(year);
		}

		constexpr uint64 DAYS_BEFORE_2000 = DaysBeforeYear(2000);
		static_assert(DAYS_BEFORE_2000 == 730485);

		uint64 ReadTimeBase()
		{
			return PPCInterpreter_getMainCoreCycleCounter() / 20ull;
		}
	}

	uint64 OSGetSystemTime()
	{
		return ReadTimeBase();
	}

	uint64 OSGetTime()
	{
		return ReadTimeBase() + ppcCyclesSince2000TimerClock;
	}

	uint32 OSGetSystemTick()
	{
		return (uint32)ReadTimeBase();
	}

	uint32 OSGetTick()
	{
		return (uint32)OSGetTime();
	}

	// Inverse of OSTicksToCalendarTime. Fields are not validated; out-of-range values are
	// folded in with plain integer arithmetic (a month above 11 wraps around).
	uint64 OSCalendarTimeToTicks(OSCalendarTime_t* calendar)
	{
		const uint32 year = (uint32)(sint32)calendar->year;
		const uint32 month = (uint32)(sint32)calendar->month % 12;
		const uint32 firstDayOfMonth = MONTH_START[IsLeap(year) ? 1 : 0][month];
		const uint64 dayOfYear = (uint32)(firstDayOfMonth + (uint32)(sint32)calendar->dayOfMonth - 1);
		const uint64 days = dayOfYear + (uint32)(year * 365) + LeapYearsBefore(year) - DAYS_BEFORE_2000;

		auto widen = [](sint32 v) { return (uint64)(sint64)v; };
		const uint64 seconds = days * SECONDS_PER_DAY + widen(calendar->hour) * 3600 + widen(calendar->minute) * 60 + widen(calendar->second);
		const uint64 fraction = widen(calendar->millisecond) * TICKS_PER_SECOND / 1000 + widen(calendar->microsecond) * TICKS_PER_SECOND / 1000000;
		return seconds * TICKS_PER_SECOND + fraction;
	}

	void OSTicksToCalendarTime(uint64 ticks, OSCalendarTime_t* calendar)
	{
		const uint64 seconds = ticks / TICKS_PER_SECOND;
		const uint64 fraction = ticks % TICKS_PER_SECOND;
		calendar->microsecond = (sint32)((fraction * 1000000 / TICKS_PER_SECOND) % 1000);
		calendar->millisecond = (sint32)((fraction * 1000 / TICKS_PER_SECOND) % 1000);

		const uint64 daysSince2000 = seconds / SECONDS_PER_DAY;
		calendar->dayOfWeek = (sint32)((daysSince2000 + 6) % 7); // 2000-01-01 was a Saturday

		uint32 secondOfDay = (uint32)(seconds % SECONDS_PER_DAY);
		calendar->second = (sint32)(secondOfDay % 60);
		calendar->minute = (sint32)(secondOfDay / 60 % 60);
		calendar->hour = (sint32)(secondOfDay / 3600);

		// estimate the year from the mean year length, then correct it
		const uint64 day = daysSince2000 + DAYS_BEFORE_2000;
		uint32 year = (uint32)(day * 400 / 146097);
		while (DaysBeforeYear(year) > day)
			year--;
		while (DaysBeforeYear(year + 1) <= day)
			year++;
		calendar->year = (sint32)year;

		const uint32 dayOfYear = (uint32)(day - DaysBeforeYear(year));
		const uint32* starts = MONTH_START[IsLeap(year) ? 1 : 0];
		uint32 month = 11;
		while (starts[month] > dayOfYear)
			month--;
		calendar->dayOfYear = (sint32)dayOfYear;
		calendar->month = (sint32)month;
		calendar->dayOfMonth = (sint32)(dayOfYear - starts[month] + 1);
	}

	void InitializeTimeAndCalendar()
	{
		cafeExportRegister("coreinit", OSGetTime, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetSystemTime, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetTick, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetSystemTick, LogType::Placeholder);

		cafeExportRegister("coreinit", OSTicksToCalendarTime, LogType::Placeholder);
		cafeExportRegister("coreinit", OSCalendarTimeToTicks, LogType::Placeholder);
	}
};
