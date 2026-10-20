// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// SIMD intrinsics for x86-64.
//
// Every supported compiler provides the intrinsics up to SSE4.2. MSVC lets any function use any
// instruction set. GCC and Clang only allow an intrinsic inside a function compiled for its
// instruction set, so functions using SSE3..SSE4.2 intrinsics carry the matching
// FUNCTION_TARGET_* attribute (empty when the whole build already targets that level, or on
// MSVC). Callers check the CPU at runtime before calling such a function.

#pragma once

#if defined(_M_X86_64)
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <x86intrin.h>

#if !defined(__SSE3__)
#define FUNCTION_TARGET_SSE3 [[gnu::target("sse3")]]
#endif
#if !defined(__SSSE3__)
#define FUNCTION_TARGET_SSSE3 [[gnu::target("ssse3")]]
#endif
#if !defined(__SSE4_1__)
#define FUNCTION_TARGET_SSR41 [[gnu::target("sse4.1")]]
#endif
#if !defined(__SSE4_2__)
#define FUNCTION_TARGET_SSE42 [[gnu::target("sse4.2")]]
#endif
#endif
#endif  // _M_X86_64

#if !defined(FUNCTION_TARGET_SSE3)
#define FUNCTION_TARGET_SSE3
#endif
#if !defined(FUNCTION_TARGET_SSSE3)
#define FUNCTION_TARGET_SSSE3
#endif
#if !defined(FUNCTION_TARGET_SSR41)
#define FUNCTION_TARGET_SSR41
#endif
#if !defined(FUNCTION_TARGET_SSE42)
#define FUNCTION_TARGET_SSE42
#endif
