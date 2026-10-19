// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// CPU time split into idle, kernel and user parts, in OS-specific units
struct ProcessorTime
{
	uint64 idle{}, kernel{}, user{};

	uint64 work();  // kernel + user
	uint64 total(); // idle + kernel + user

	// busy fraction (0..1) between two samples
	static double Compare(ProcessorTime& last, ProcessorTime& now);
};

uint32 GetProcessorCount();
// resident memory of this process in bytes (0 if unknown)
uint64 QueryRamUsage();
// wall clock plus this process's user and kernel CPU time, in the same units
void QueryProcTime(uint64& out_now, uint64& out_user, uint64& out_kernel);
void QueryProcTime(ProcessorTime& out);
// per-core times for the first out.size() cores (count is the number of cores)
void QueryCoreTimes(uint32 count, std::vector<ProcessorTime>& out);
