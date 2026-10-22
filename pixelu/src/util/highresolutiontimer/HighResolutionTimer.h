// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

using HRTick = uint64;

// Monotonic high resolution clock reading. Ticks count at getFrequency() per
// second (nanoseconds on Linux, macOS and BSD; the performance counter on Windows).
class HighResolutionTimer
{
public:
	HighResolutionTimer() : m_timePoint(0) {}

	static HighResolutionTimer now();
	static HRTick getFrequency();

	HRTick getTick() const { return m_timePoint; }
	uint64 getTickInSeconds() const { return m_timePoint / getFrequency(); }

	// elapsed seconds as double; meant for statistics and logging, not exact timing
	static double getTimeDiff(HRTick startTime, HRTick endTime)
	{
		return (double)(endTime - startTime) / (double)getFrequency();
	}

	// elapsed ticks, plus the tick frequency in freq
	static uint64 getTimeDiffEx(HRTick startTime, HRTick endTime, uint64& freq)
	{
		freq = getFrequency();
		return endTime - startTime;
	}

	static HRTick microsecondsToTicks(uint64 microseconds) { return microseconds * getFrequency() / 1000000; }
	static uint64 ticksToMicroseconds(HRTick ticks) { return ticks * 1000000 / getFrequency(); }

private:
	explicit HighResolutionTimer(uint64 timePoint) : m_timePoint(timePoint) {}

	uint64 m_timePoint;
};

// stopwatch: time between Start() and Stop()
class BenchmarkTimer
{
public:
	void Start() { m_start = HighResolutionTimer::now().getTick(); }
	void Stop() { m_stop = HighResolutionTimer::now().getTick(); }

	double GetElapsedMilliseconds() const
	{
		cemu_assert_debug(m_start != 0 && m_stop != 0);
		cemu_assert_debug(m_start <= m_stop);
		return (double)(m_stop - m_start) * 1000.0 / (double)HighResolutionTimer::getFrequency();
	}

private:
	HRTick m_start{};
	HRTick m_stop{};
};

// Scoped timer for profiling: the time spent in all scopes with the same name
// is summed up and FrameEnd() logs the per-frame average every 100 frames.
class FrameBenchmarkHelper
{
public:
	FrameBenchmarkHelper(std::string_view name);
	~FrameBenchmarkHelper();

	FrameBenchmarkHelper(const FrameBenchmarkHelper&) = delete;
	FrameBenchmarkHelper& operator=(const FrameBenchmarkHelper&) = delete;

	static void FrameEnd();

	struct Entry;
	Entry* m_entry;
};
