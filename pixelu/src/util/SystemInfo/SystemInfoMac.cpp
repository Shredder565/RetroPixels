// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/SystemInfo/SystemInfo.h"

#include <unistd.h>
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <mach/mach_host.h>
#include <mach/kern_return.h>

uint64 QueryRamUsage()
{
	mach_task_basic_info_data_t info{};
	mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &infoCount) != KERN_SUCCESS)
		return 0;
	return info.resident_size;
}

void QueryCoreTimes(uint32 count, std::vector<ProcessorTime>& out)
{
	(void)count;
	for (auto& t : out)
		t = {};

	natural_t cpuCount = 0;
	processor_info_array_t infoArray = nullptr;
	mach_msg_type_number_t infoCount = 0;
	const mach_port_t host = mach_host_self();
	const kern_return_t kr = host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &cpuCount, &infoArray, &infoCount);
	mach_port_deallocate(mach_task_self(), host);
	if (kr != KERN_SUCCESS)
		return;

	const auto* load = reinterpret_cast<const processor_cpu_load_info_data_t*>(infoArray);
	const size_t n = std::min<size_t>(cpuCount, out.size());
	for (size_t i = 0; i < n; i++)
	{
		out[i].user = (uint64)load[i].cpu_ticks[CPU_STATE_USER] + (uint64)load[i].cpu_ticks[CPU_STATE_NICE];
		out[i].kernel = load[i].cpu_ticks[CPU_STATE_SYSTEM];
		out[i].idle = load[i].cpu_ticks[CPU_STATE_IDLE];
	}

	// the kernel allocated the array in our address space
	if (vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(infoArray), infoCount * sizeof(integer_t)) != KERN_SUCCESS)
		cemuLog_log(LogType::Force, "vm_deallocate() failed");
}
