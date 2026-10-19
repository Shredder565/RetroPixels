// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/SystemInfo/SystemInfo.h"

#include <sys/times.h>

// all three values in clock ticks (sysconf(_SC_CLK_TCK) per second)
void QueryProcTime(uint64& out_now, uint64& out_user, uint64& out_kernel)
{
	struct tms cpu;
	const clock_t wall = times(&cpu);
	out_now = (uint64)wall;
	out_user = (uint64)cpu.tms_utime;
	out_kernel = (uint64)cpu.tms_stime;
}
