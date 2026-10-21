// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Precompiled header for the MSVC build: headers used across most of the code base. On Windows
// it also stops early with a clear message when the compiler or SDK is too old.

#ifdef _WIN32

#define PCH_QUOTE_(x) #x
#define PCH_QUOTE(x) PCH_QUOTE_(x)

// Visual Studio 2026 or newer.
#if defined _MSC_FULL_VER && _MSC_FULL_VER < 195136252
#pragma message("Current _MSC_FULL_VER: " PCH_QUOTE(_MSC_FULL_VER))
#error Please update your build environment to the latest Visual Studio 2026!
#endif

// Windows SDK 10.0.22621 (the first to define NTDDI_WIN10_NI) or newer.
#include <sdkddkver.h>
#ifndef NTDDI_WIN10_NI
#pragma message("Current WDK_NTDDI_VERSION: " PCH_QUOTE(WDK_NTDDI_VERSION))
#error Windows 10.0.22621 SDK or later is required
#endif

#undef PCH_QUOTE
#undef PCH_QUOTE_

#endif  // _WIN32

// C++ library.
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// C library, under its C names too, since some code uses those.
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <memory.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fmt/format.h>

// Platform headers.
#ifdef _WIN32
#ifdef _M_X86_64
#include <intrin.h>
#endif
#include <windows.h>
#else
#ifndef ANDROID
#include <execinfo.h>
#endif
#include <getopt.h>
#include <pthread.h>
#include <unistd.h>
#endif

#include "Common/Common.h"
#include "Common/Thread.h"
