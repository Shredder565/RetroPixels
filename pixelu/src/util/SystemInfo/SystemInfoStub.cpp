// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/SystemInfo/SystemInfo.h"

// platforms without process statistics report zeros

uint64 QueryRamUsage()
{
	return 0;
}

void QueryProcTime(uint64& out_now, uint64& out_user, uint64& out_kernel)
{
	out_now = out_user = out_kernel = 0;
}

void QueryCoreTimes(uint32 count, std::vector<ProcessorTime>& out)
{
	(void)count;
	for (auto& t : out)
		t = {};
}
