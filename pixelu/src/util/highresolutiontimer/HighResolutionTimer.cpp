// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/highresolutiontimer/HighResolutionTimer.h"

#if !BOOST_OS_WINDOWS
#include <time.h>
#endif

HighResolutionTimer HighResolutionTimer::now()
{
#if BOOST_OS_WINDOWS
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return HighResolutionTimer((uint64)counter.QuadPart);
#elif BOOST_OS_MACOS
	return HighResolutionTimer(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW));
#else
#if BOOST_OS_LINUX
	constexpr clockid_t clockId = CLOCK_MONOTONIC_RAW;
#else
	constexpr clockid_t clockId = CLOCK_MONOTONIC;
#endif
	timespec ts;
	clock_gettime(clockId, &ts);
	return HighResolutionTimer((uint64)ts.tv_sec * 1000000000ull + (uint64)ts.tv_nsec);
#endif
}

HRTick HighResolutionTimer::getFrequency()
{
#if BOOST_OS_WINDOWS
	// queried on first use so that it is valid during static initialization too
	static const uint64 s_frequency = []() {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return (uint64)f.QuadPart;
	}();
	return s_frequency;
#else
	return 1000000000ull; // now() reports nanoseconds
#endif
}

struct FrameBenchmarkHelper::Entry
{
	HRTick totalTicks{};
	HRTick startTick{};
};

namespace
{
	constexpr uint32 kReportInterval = 100;

	struct FrameBenchmarkState
	{
		std::map<std::string, FrameBenchmarkHelper::Entry, std::less<>> entries;
		uint32 frames{ 0 };
	};

	FrameBenchmarkState& benchmarkState()
	{
		static FrameBenchmarkState s;
		return s;
	}
}

FrameBenchmarkHelper::FrameBenchmarkHelper(std::string_view name)
{
	auto& entries = benchmarkState().entries;
	auto it = entries.find(name);
	if (it == entries.end())
		it = entries.emplace(std::string(name), Entry{}).first;
	m_entry = &it->second;
	m_entry->startTick = HighResolutionTimer::now().getTick();
}

FrameBenchmarkHelper::~FrameBenchmarkHelper()
{
	m_entry->totalTicks += HighResolutionTimer::now().getTick() - m_entry->startTick;
}

void FrameBenchmarkHelper::FrameEnd()
{
	FrameBenchmarkState& state = benchmarkState();
	if (state.entries.empty())
		return;
	if (++state.frames < kReportInterval)
		return;
	state.frames = 0;
	cemuLog_log(LogType::Force, "FrameBenchmarkHelper results (avg over last 100 frames):");
	for (auto& [name, entry] : state.entries)
	{
		const double msPerFrame = HighResolutionTimer::getTimeDiff(0, entry.totalTicks) * 1000.0 / (double)kReportInterval;
		entry.totalTicks = 0;
		cemuLog_log(LogType::Force, "  {}: {:.4f} ms/frame", name, msPerFrame);
	}
}
