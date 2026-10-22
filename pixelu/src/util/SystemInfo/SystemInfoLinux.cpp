// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/SystemInfo/SystemInfo.h"

#include <unistd.h>

uint64 QueryRamUsage()
{
	static const long s_pageSize = sysconf(_SC_PAGESIZE);
	if (s_pageSize <= 0)
		return 0;
	// /proc/self/statm: total program size, then resident pages, ...
	std::ifstream statm("/proc/self/statm");
	uint64 totalPages = 0, residentPages = 0;
	if (!(statm >> totalPages >> residentPages))
		return 0;
	return residentPages * (uint64)s_pageSize;
}

void QueryCoreTimes(uint32 count, std::vector<ProcessorTime>& out)
{
	(void)count;
	for (auto& t : out)
		t = {};
	// /proc/stat: an aggregate "cpu" line, then "cpuN user nice system idle ..." per core
	std::ifstream stat("/proc/stat");
	if (!stat)
		return;
	std::string line;
	std::getline(stat, line); // aggregate
	for (auto& t : out)
	{
		if (!std::getline(stat, line))
			break;
		std::istringstream fields(line);
		std::string label;
		uint64 user = 0, nice = 0, system = 0, idle = 0;
		fields >> label >> user >> nice >> system >> idle;
		t.user = user + nice;
		t.kernel = system;
		t.idle = idle;
	}
}
