// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Small Windows-API equivalents for Linux, BSD and macOS.

#include <shared_mutex>
#include <cerrno>
#include <cstddef>
#include <cstdint>

// reader/writer lock with the interface of the Windows SRW lock wrapper
class SlimRWLock
{
public:
	void LockRead() { m_mutex.lock_shared(); }
	void UnlockRead() { m_mutex.unlock_shared(); }
	void LockWrite() { m_mutex.lock(); }
	void UnlockWrite() { m_mutex.unlock(); }

private:
	std::shared_mutex m_mutex;
};

inline uint32_t GetExceptionError()
{
	return (uint32_t)errno;
}

// X11 headers define these as macros, which collide with names in the code base
#undef False
#undef True
#undef None
#undef Bool
#undef Status
#undef Success
#undef ClientMessage

// milliseconds from a monotonic clock
uint32_t GetTickCount();

// Bounded string copy into a char array: copies at most N-1 characters and
// always terminates the result.
template<size_t N>
void strcpy_s(char (&dst)[N], const char* src)
{
	if constexpr (N == 0)
		return;
	size_t i = 0;
	for (; i < N - 1 && src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

// Bounded concatenation into a char array: appends as much of src as fits
// and always terminates the result.
template<size_t N>
void strcat_s(char (&dst)[N], const char* src)
{
	if constexpr (N == 0)
		return;
	size_t pos = 0;
	while (pos < N - 1 && dst[pos] != '\0')
		pos++;
	for (size_t i = 0; pos < N - 1 && src[i] != '\0'; i++, pos++)
		dst[pos] = src[i];
	dst[pos] = '\0';
}
