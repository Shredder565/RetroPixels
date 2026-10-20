// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Fixed-size integer names and clock aliases used throughout the emulator.

#pragma once

#include <chrono>
#include <cstdint>

#ifdef _WIN32
#include <tchar.h>
#else
// Stand-ins so code written against the Win32 names still compiles elsewhere.
#define TCHAR char
#define LONG int
#endif

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Monotonic clock used for frame pacing and performance measurements.
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using DT = Clock::duration;

// Floating-point durations, handy for statistics.
using DT_s = std::chrono::duration<double>;
using DT_ms = std::chrono::duration<double, std::milli>;
using DT_us = std::chrono::duration<double, std::micro>;
