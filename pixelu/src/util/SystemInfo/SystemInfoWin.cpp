// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/SystemInfo/SystemInfo.h"

#include <Psapi.h>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")

namespace
{
	uint64 toU64(const FILETIME& ft)
	{
		return ((uint64)ft.dwHighDateTime << 32) | (uint64)ft.dwLowDateTime;
	}
}

uint64 QueryRamUsage()
{
	PROCESS_MEMORY_COUNTERS counters{};
	counters.cb = sizeof(counters);
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
		return 0;
	return counters.WorkingSetSize;
}

// all three values in 100ns units
void QueryProcTime(uint64& out_now, uint64& out_user, uint64& out_kernel)
{
	FILETIME wall, creation, exitTime, kernel, user;
	GetSystemTimeAsFileTime(&wall);
	if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user))
	{
		out_now = out_user = out_kernel = 0;
		return;
	}
	out_now = toU64(wall);
	out_user = toU64(user);
	out_kernel = toU64(kernel);
}

void QueryCoreTimes(uint32 count, std::vector<ProcessorTime>& out)
{
	for (auto& t : out)
		t = {};
	std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> perf(count);
	const ULONG bytes = (ULONG)(perf.size() * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION));
	if (!NT_SUCCESS(NtQuerySystemInformation(SystemProcessorPerformanceInformation, perf.data(), bytes, nullptr)))
		return;
	const size_t n = std::min<size_t>(perf.size(), out.size());
	for (size_t i = 0; i < n; i++)
	{
		// KernelTime includes the idle time
		const uint64 idle = (uint64)perf[i].IdleTime.QuadPart;
		out[i].idle = idle;
		out[i].kernel = (uint64)perf[i].KernelTime.QuadPart - idle;
		out[i].user = (uint64)perf[i].UserTime.QuadPart;
	}
}
