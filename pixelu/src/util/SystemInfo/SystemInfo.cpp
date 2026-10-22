// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "util/SystemInfo/SystemInfo.h"

uint64 ProcessorTime::work()
{
	return kernel + user;
}

uint64 ProcessorTime::total()
{
	return idle + kernel + user;
}

double ProcessorTime::Compare(ProcessorTime& last, ProcessorTime& now)
{
	const uint64 busy = now.work() - last.work();
	const uint64 all = now.total() - last.total();
	return (double)busy / (double)all;
}

uint32 GetProcessorCount()
{
	return std::thread::hardware_concurrency();
}

void QueryProcTime(ProcessorTime& out)
{
	uint64 wall, user, kernel;
	QueryProcTime(wall, user, kernel);
	out.user = user;
	out.kernel = kernel;
	out.idle = wall - (user + kernel);
}
