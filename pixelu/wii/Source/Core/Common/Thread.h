// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <thread>

#ifndef _WIN32
#include <cstddef>
#include <tuple>
#endif

// Only CommonTypes.h here: LogManager includes this header and Common.h would get in the way.
#include "Common/CommonTypes.h"

#if !defined(_WIN32) && !defined(INFINITE)
#define INFINITE 0xffffffff
#endif

namespace Common
{
// OS thread id of the calling thread (0 where the platform has no cheap equivalent).
int CurrentThreadId();

// `mask` bit n allows CPU n.
void SetThreadAffinity(std::thread::native_handle_type thread, u32 mask);
void SetCurrentThreadAffinity(u32 mask);

void SleepCurrentThread(int ms);
// Gives up the rest of the time slice (on POSIX: sleeps for 1 ms).
void SwitchCurrentThread();

// For spin-waits: lets other threads run without a kernel wait.
inline void YieldCPU()
{
  std::this_thread::yield();
}

// Names the calling thread for debuggers and profilers.
void SetCurrentThreadName(const char* name);

#ifndef _WIN32
// The lowest address of the calling thread's stack, and its size.
std::tuple<void*, size_t> GetCurrentThreadStack();
#endif
}  // namespace Common
