// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Build configuration for the liblzma sources of XZ Utils 5.2.4 (fetched from
// the XZ Utils repository, see Externals/sources.json), for PixelU-Wii's
// Windows x64 and Linux x86-64 builds. XZ Utils normally
// generates this file with its configure script; these are the answers for MSVC
// on 64-bit Windows. Only the library is built, so nothing for the xz tools
// (gettext, getopt, file times, ...) is here.

#pragma once

// Integrity checks.
#define HAVE_CHECK_CRC32 1
#define HAVE_CHECK_CRC64 1
#define HAVE_CHECK_SHA256 1

// Filters: LZMA1, LZMA2 and Delta, both ways (no BCJ filters).
#define HAVE_DECODERS 1
#define HAVE_DECODER_LZMA1 1
#define HAVE_DECODER_LZMA2 1
#define HAVE_DECODER_DELTA 1
#define HAVE_ENCODERS 1
#define HAVE_ENCODER_LZMA1 1
#define HAVE_ENCODER_LZMA2 1
#define HAVE_ENCODER_DELTA 1

// Match finders.
#define HAVE_MF_HC3 1
#define HAVE_MF_HC4 1
#define HAVE_MF_BT2 1
#define HAVE_MF_BT3 1
#define HAVE_MF_BT4 1

#ifdef _WIN32
// Threads: Windows Vista condition variables.
#define MYTHREAD_VISTA 1
#else
// Linux (glibc 2.35+): POSIX threads with a monotonic clock for timed waits, the CPU
// count from the affinity mask and the RAM size from sysconf.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define MYTHREAD_POSIX 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_DECL_CLOCK_MONOTONIC 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#define TUKLIB_CPUCORES_SCHED_GETAFFINITY 1
#define TUKLIB_PHYSMEM_SYSCONF 1
#define HAVE_UNISTD_H 1
#endif

// The C library.
#define STDC_HEADERS 1
#define HAVE_STDBOOL_H 1
#define HAVE__BOOL 1
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMORY_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UINTPTR_T 1

// 64-bit x86: size_t is 8 bytes, unaligned loads are cheap, SSE2 intrinsics exist.
#define SIZEOF_SIZE_T 8
#ifdef _M_X86_64
#define TUKLIB_FAST_UNALIGNED_ACCESS 1
#define HAVE_IMMINTRIN_H 1
#endif
#define HAVE__MM_MOVEMASK_EPI8 1

// tuklib_integer.h uses DWORD and the _BitScan* intrinsics when built with
// MSVC, without including the headers that declare them.
#ifdef _MSC_VER
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#endif

// Memory to assume when the real amount cannot be read.
#define ASSUME_RAM 128

// liblzma's internal assertions stay off in every configuration.
#ifndef NDEBUG
#define NDEBUG 1
#endif

#define PACKAGE "xz"
#define PACKAGE_NAME "XZ Utils"
#define PACKAGE_VERSION "5.2.4"
#define PACKAGE_STRING "XZ Utils 5.2.4"
#define PACKAGE_TARNAME "xz"
#define PACKAGE_BUGREPORT "lasse.collin@tukaani.org"
#define PACKAGE_URL "https://tukaani.org/xz/"
#define VERSION "5.2.4"
