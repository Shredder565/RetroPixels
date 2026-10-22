// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include <cstdint>
#include <ctime>

uint32_t GetTickCount()
{
#if BOOST_OS_MACOS
	return (uint32_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000000);
#else
	#if BOOST_OS_LINUX
	constexpr clockid_t clockId = CLOCK_MONOTONIC_RAW;
	#else
	constexpr clockid_t clockId = CLOCK_MONOTONIC;
	#endif
	timespec now{};
	clock_gettime(clockId, &now);
	// the value wraps around like the Windows function
	return (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
#endif
}
