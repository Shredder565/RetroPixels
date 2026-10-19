// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Prefix header included by every translation unit: standard library and
// third-party includes, the basic integer types, endian helpers, compiler
// abstraction macros, asserts and small utilities.

#include <stdlib.h>

#include "version.h"
#include "platform.h"

#define FMT_HEADER_ONLY
#define FMT_USE_GRISU 1
#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#if FMT_VERSION > 80000
#include <fmt/xchar.h> // wchar_t formatting
#endif

#define SDL_MAIN_HANDLED

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#define ARCH_X86_64
#endif

#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <cassert>

#if defined(ARCH_X86_64)
#include <immintrin.h>
#endif

#include <string>
#include <string_view>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <future>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <queue>
#include <stack>
#include <array>
#include <bitset>
#include <bit>
#include <filesystem>
#include <memory>
#include <chrono>
#include <regex>
#include <type_traits>
#include <optional>
#include <span>
#include <ranges>
#include <variant>

#include <boost/predef.h>
#include <boost/nowide/convert.hpp>
#include <boost/algorithm/string.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fs = std::filesystem;

#include "enumFlags.h"

// ---- integer types ----

using uint64 = uint64_t;
using uint32 = uint32_t;
using uint16 = uint16_t;
using uint8 = uint8_t;

using sint64 = int64_t;
using sint32 = int32_t;
using sint16 = int16_t;
using sint8 = int8_t;

#if _MSC_VER
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#endif
#endif

// big-endian storage types (betype<T>, uint32be, ...)
#include "betype.h"

// little-endian storage types; the host is little-endian, so these are plain integers
using uint64le = uint64_t;
using uint32le = uint32_t;
using uint16le = uint16_t;
using uint8le = uint8_t;

#include "Cemu/Logging/CemuDebugLogging.h"
#include "Cemu/Logging/CemuLogging.h"

// ---- UI text translation ----
// The callback lives in an unnamed namespace, so every translation unit has
// its own copy: SetTranslationCallback() only affects _tr() calls made from
// the same source file.

namespace
{
	std::function<std::string(std::string_view)> g_translate;
}

inline void SetTranslationCallback(std::function<std::string(std::string_view)> translate)
{
	g_translate = translate;
}

#define TR_NOOP(str) str

inline std::string _tr(std::string_view text)
{
	return g_translate ? g_translate(text) : std::string(text);
}

// the translated text is used as the format string
template<typename... TArgs>
inline std::string _tr(fmt::format_string<TArgs...> text, TArgs... args)
{
	if (!g_translate)
		return fmt::format(text, std::forward<TArgs>(args)...);
	const auto raw = text.get();
	return fmt::format(fmt::runtime(g_translate(std::string_view(raw.data(), raw.size()))), std::forward<TArgs>(args)...);
}

// ---- byte swapping ----

inline uint16 _swapEndianU16(uint16 v)
{
	return (uint16)((v >> 8) | (v << 8));
}

inline sint16 _swapEndianS16(sint16 v)
{
	return (sint16)_swapEndianU16((uint16)v);
}

#if _MSC_VER
inline uint64 _swapEndianU64(uint64 v) { return _byteswap_uint64(v); }
inline uint32 _swapEndianU32(uint32 v) { return _byteswap_ulong(v); }
#elif BOOST_OS_MACOS
inline uint64 _swapEndianU64(uint64 v) { return OSSwapInt64(v); }
inline uint32 _swapEndianU32(uint32 v) { return OSSwapInt32(v); }
#elif BOOST_OS_BSD && defined(__OpenBSD__)
inline uint64 _swapEndianU64(uint64 v) { return swap64(v); }
inline uint32 _swapEndianU32(uint32 v) { return swap32(v); }
#elif BOOST_OS_BSD
inline uint64 _swapEndianU64(uint64 v) { return bswap64(v); }
inline uint32 _swapEndianU32(uint32 v) { return bswap32(v); }
#else
inline uint64 _swapEndianU64(uint64 v) { return bswap_64(v); }
inline uint32 _swapEndianU32(uint32 v) { return bswap_32(v); }
#endif

inline sint32 _swapEndianS32(sint32 v)
{
	return (sint32)_swapEndianU32((uint32)v);
}

// ---- stand-ins for MSVC / Windows SDK definitions on other compilers ----

#if !_MSC_VER
// 64x64 -> 128 bit multiplication: returns the low half, stores the high half
inline uint64 _umul128(uint64 multiplier, uint64 multiplicand, uint64* highProduct)
{
	const unsigned __int128 product = (unsigned __int128)multiplier * multiplicand;
	*highProduct = (uint64)(product >> 64);
	return (uint64)product;
}

typedef uint8_t BYTE;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int64_t LONGLONG;

typedef union _LARGE_INTEGER {
	struct {
		DWORD LowPart;
		LONG HighPart;
	};
	struct {
		DWORD LowPart;
		LONG HighPart;
	} u;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

// bitwise operators for a scoped enum used as flag set
#define DEFINE_ENUM_FLAG_OPERATORS(T) \
	inline T operator~(T a) { return static_cast<T>(~static_cast<std::underlying_type<T>::type>(a)); } \
	inline T operator|(T a, T b) { return static_cast<T>(static_cast<std::underlying_type<T>::type>(a) | static_cast<std::underlying_type<T>::type>(b)); } \
	inline T operator&(T a, T b) { return static_cast<T>(static_cast<std::underlying_type<T>::type>(a) & static_cast<std::underlying_type<T>::type>(b)); } \
	inline T operator^(T a, T b) { return static_cast<T>(static_cast<std::underlying_type<T>::type>(a) ^ static_cast<std::underlying_type<T>::type>(b)); } \
	inline T& operator|=(T& a, T b) { return a = (a | b); } \
	inline T& operator&=(T& a, T b) { return a = (a & b); } \
	inline T& operator^=(T& a, T b) { return a = (a ^ b); }
#endif

#if !defined(_MSC_VER) || defined(__clang__) // clang-cl lacks the _udiv128 intrinsic
// 128 / 64 bit division: returns the (truncated) quotient, stores the remainder
inline uint64 _udiv128(uint64 highDividend, uint64 lowDividend, uint64 divisor, uint64* remainder)
{
	const unsigned __int128 dividend = ((unsigned __int128)highDividend << 64) | lowDividend;
	*remainder = (uint64)(dividend % divisor);
	return (uint64)(dividend / divisor);
}
#endif

// ---- bit fields ----
// numBits must be below 32 (the mask is computed in int)

template<typename T>
inline T GetBits(T value, uint32 index, uint32 numBits)
{
	const T mask = (T)((1 << numBits) - 1);
	return (value >> index) & mask;
}

template<typename T>
inline void SetBits(T& value, uint32 index, uint32 numBits, uint32 bitValue)
{
	const T mask = (T)((1 << numBits) - 1);
	value &= ~(mask << index);
	value |= (bitValue << index);
}

// ---- compiler abstraction ----

#if defined(_MSC_VER)
#define UNREACHABLE __assume(false)
#define ASSUME(__cond) __assume(__cond)
#define TLS_WORKAROUND_NOINLINE // MSVC has fiber-safe TLS optimizations as a compiler option
#elif defined(__GNUC__) && !defined(__llvm__)
#define UNREACHABLE __builtin_unreachable()
#define ASSUME(__cond) __attribute__((assume(__cond)))
#define TLS_WORKAROUND_NOINLINE __attribute__((noinline))
#elif defined(__clang__)
#define UNREACHABLE __builtin_unreachable()
#define ASSUME(__cond) __builtin_assume(__cond)
#define TLS_WORKAROUND_NOINLINE __attribute__((noinline))
#else
#error Unknown compiler
#endif

#if defined(_MSC_VER)
#define DEBUG_BREAK __debugbreak()
#else
#include <csignal>
#define DEBUG_BREAK raise(SIGTRAP)
#endif

#if defined(_MSC_VER)
#define DLLEXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#if BOOST_OS_WINDOWS
#define DLLEXPORT __attribute__((dllexport))
#else
#define DLLEXPORT
#endif
#else
#error No definition for DLLEXPORT
#endif

#if BOOST_OS_WINDOWS
#define NOEXPORT
#elif defined(__GNUC__)
#define NOEXPORT __attribute__((visibility("hidden")))
#endif

#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#else
#define FORCE_INLINE inline
#endif

// index of the lowest set bit; undefined for 0
FORCE_INLINE int BSF(uint32 v)
{
#if defined(_MSC_VER)
	return _tzcnt_u32(v); // runs as BSF on CPUs without BMI1
#elif defined(__GNUC__) || defined(__clang__)
	return __builtin_ctz(v);
#else
	return std::countr_zero(v);
#endif
}

// x86 intrinsics used by the code base, emulated on aarch64
#if defined(__aarch64__)
inline void _mm_pause()
{
	asm volatile("yield");
}

inline uint64 __rdtsc()
{
	uint64 ticks;
	asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
	return ticks;
}

inline void _mm_mfence()
{
	asm volatile("" ::: "memory");
	std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline unsigned char _addcarry_u64(unsigned char carry, unsigned long long a, unsigned long long b, unsigned long long* result)
{
	*result = a + b + (unsigned long long)carry;
	return *result < a ? 1 : 0;
}
#endif

// ---- asserts ----
// cemu_assert and cemu_assert_error always break; the other variants only
// with CEMU_DEBUG_ASSERT. The condition is evaluated in every build.

inline void cemu_assert(bool _condition)
{
	if (!_condition)
		DEBUG_BREAK;
}

#ifdef CEMU_DEBUG_ASSERT
inline void cemu_assert_debug(bool _condition)
{
	if (!_condition)
		DEBUG_BREAK;
}
inline void cemu_assert_unimplemented() { DEBUG_BREAK; }
inline void cemu_assert_suspicious() { DEBUG_BREAK; }
#else
inline void cemu_assert_debug(bool _condition) { (void)_condition; }
inline void cemu_assert_unimplemented() {}
inline void cemu_assert_suspicious() {}
#endif

inline void cemu_assert_error()
{
	DEBUG_BREAK;
}

#define assert_dbg() DEBUG_BREAK // unconditional break

#include "Common/MemPtr.h"

template<typename T1, typename T2>
constexpr bool HAS_FLAG(T1 flags, T2 test_flag) { return (flags & (T1)test_flag) == (T1)test_flag; }
template<typename T1, typename T2>
constexpr bool HAS_BIT(T1 value, T2 index) { return (value & ((T1)1 << index)) != 0; }

// size of a type as seen by guest code
template<typename T>
constexpr uint32_t ppcsizeof() { return (uint32_t)sizeof(T); }

// ---- small container helpers ----

// adds val unless it is already present (for small vectors used as sets)
template<typename T>
void vectorAppendUnique(std::vector<T>& vec, const T& val)
{
	if (std::find(vec.begin(), vec.end(), val) == vec.end())
		vec.emplace_back(val);
}

template<typename T>
void vectorRemoveByValue(std::vector<T>& vec, const T& val)
{
	vec.erase(std::remove(vec.begin(), vec.end(), val), vec.end());
}

template<typename T>
void vectorRemoveByIndex(std::vector<T>& vec, const size_t index)
{
	vec.erase(vec.begin() + index);
}

// true if value equals any of the others
template<typename T1, typename... Types>
bool match_any_of(T1&& value, Types&&... others)
{
	return (... || (value == others));
}

// ---- clocks ----
// On Windows the performance counter is read directly (its frequency is
// queried once) instead of going through the standard clocks.

#ifdef _WIN32
namespace detail_clock
{
	inline long long perfCounterToNanoseconds()
	{
		static const long long s_frequency = _Query_perf_frequency();
		const long long counter = _Query_perf_counter();
		static_assert(std::nano::num == 1);
		// split to avoid overflowing counter * 1e9
		return (counter / s_frequency) * std::nano::den + (counter % s_frequency) * std::nano::den / s_frequency;
	}
}
#endif

[[nodiscard]] static std::chrono::high_resolution_clock::time_point now_cached() noexcept
{
#ifdef _WIN32
	return std::chrono::high_resolution_clock::time_point(std::chrono::nanoseconds(detail_clock::perfCounterToNanoseconds()));
#else
	return std::chrono::high_resolution_clock::now();
#endif
}

[[nodiscard]] static std::chrono::steady_clock::time_point tick_cached() noexcept
{
#if BOOST_OS_WINDOWS
	return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(detail_clock::perfCounterToNanoseconds()));
#elif BOOST_OS_MACOS
	return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)));
#else
#if BOOST_OS_LINUX
	constexpr clockid_t clockId = CLOCK_MONOTONIC_RAW;
#else
	constexpr clockid_t clockId = CLOCK_MONOTONIC;
#endif
	struct timespec ts;
	clock_gettime(clockId, &ts);
	return std::chrono::steady_clock::time_point(std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec));
#endif
}

// ---- UTF-8 helpers ----
// std::u8string is awkward to use with most libraries, so UTF-8 text is kept
// in std::string and converted at the boundaries.

// reinterprets UTF-8 code units as char
inline const char* _utf8WrapperPtr(const char8_t* input)
{
	return reinterpret_cast<const char*>(input);
}

inline std::string_view _utf8Wrapper(std::u8string_view input)
{
	return std::string_view(reinterpret_cast<const char*>(input.data()), input.size());
}

// path as UTF-8 with forward slashes
inline std::string _pathToUtf8(const fs::path& path)
{
	const std::u8string utf8 = path.generic_u8string();
	return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

inline fs::path _utf8ToPath(std::string_view input)
{
	return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(input.data()), input.size()));
}

// ASCII-only lower case (independent of the C locale, as on the Wii U)
inline char _ansiToLower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Runs a function during static initialization. Prefer explicit calls: other
// linkers than MSVC's may drop object files nothing references.
class RunAtCemuBoot
{
public:
	RunAtCemuBoot(void(*f)())
	{
		f();
	}
};

// true if the future's result is available without waiting
template<typename T>
bool future_is_ready(std::future<T>& f)
{
#if defined(__GNUC__)
	return f.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready;
#else
	return f._Is_ready();
#endif
}

// Views a plain variable as std::atomic. Not sanctioned by the standard, but
// works on all supported compilers when size and alignment match.
template<typename T>
std::atomic<T>* _rawPtrToAtomic(T* ptr)
{
	static_assert(sizeof(T) == sizeof(std::atomic<T>));
	cemu_assert_debug((reinterpret_cast<std::uintptr_t>(ptr) % alignof(std::atomic<T>)) == 0);
	return reinterpret_cast<std::atomic<T>*>(ptr);
}

#if defined(__GNUC__)
#define ATTR_MS_ABI __attribute__((ms_abi))
#else
#define ATTR_MS_ABI
#endif

#if !defined(TRUE) || !defined(FALSE)
#define TRUE (1)
#define FALSE (0)
#endif

inline uint32 GetTitleIdHigh(uint64 titleId)
{
	return (uint32)(titleId >> 32);
}

inline uint32 GetTitleIdLow(uint64 titleId)
{
	return (uint32)titleId;
}

#if defined(__GNUC__)
#define memcpy_dwords(__dest, __src, __numDwords) memcpy((__dest), (__src), (__numDwords) * sizeof(uint32))
#define memcpy_qwords(__dest, __src, __numQwords) memcpy((__dest), (__src), (__numQwords) * sizeof(uint64))
#else
#define memcpy_dwords(__dest, __src, __numDwords) __movsd((unsigned long*)(__dest), (const unsigned long*)(__src), __numDwords)
#define memcpy_qwords(__dest, __src, __numQwords) __movsq((unsigned long long*)(__dest), (const unsigned long long*)(__src), __numQwords)
#endif

// guest memory and CPU state
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/PPCCallback.h"

// logs the guest call stack of a thread
void DebugLogStackTrace(struct OSThread_t* thread, MPTR sp);

// enums are formatted as their underlying integer
template<typename Enum>
	requires std::is_enum_v<Enum>
struct fmt::formatter<Enum> : fmt::formatter<std::underlying_type_t<Enum>>
{
	auto format(const Enum& e, format_context& ctx) const
	{
		return fmt::formatter<std::underlying_type_t<Enum>>::format(static_cast<std::underlying_type_t<Enum>>(e), ctx);
	}
};

// betype<T> is formatted like T
template<typename T>
struct fmt::formatter<betype<T>> : fmt::formatter<T>
{
	auto format(const betype<T>& e, format_context& ctx) const
	{
		return fmt::formatter<T>::format(static_cast<T>(e), ctx);
	}
};

// stand-ins for newer standard library features
namespace stdx
{
	template<typename EnumT>
		requires (std::is_enum_v<EnumT>)
	constexpr std::underlying_type_t<EnumT> to_underlying(EnumT e) noexcept
	{
		return static_cast<std::underlying_type_t<EnumT>>(e);
	}

	// calls the function on destruction unless release() was called
	template<typename Fn>
	class scope_exit
	{
	public:
		explicit scope_exit(Fn&& f) noexcept : m_onExit(std::forward<Fn>(f)) {}
		scope_exit(scope_exit&& other) noexcept : m_onExit(std::move(other.m_onExit)), m_active(std::exchange(other.m_active, false)) {}
		scope_exit(const scope_exit&) = delete;
		scope_exit& operator=(scope_exit) = delete;

		~scope_exit()
		{
			if (m_active)
				m_onExit();
		}

		void release() { m_active = false; }

	private:
		Fn m_onExit;
		bool m_active{ true };
	};

#ifdef __cpp_lib_atomic_ref
	template<typename T>
	using atomic_ref = std::atomic_ref<T>;
#else
	// load/store-only fallback for standard libraries without std::atomic_ref
	template<typename T>
	class atomic_ref
	{
		static_assert(std::is_trivially_copyable_v<T>, "atomic_ref requires trivially copyable types");

	public:
		using value_type = T;

		explicit atomic_ref(T& obj) noexcept : m_target(std::addressof(obj)) {}

		T load(std::memory_order order = std::memory_order_seq_cst) const noexcept
		{
			return reinterpret_cast<std::atomic<T>*>(m_target)->load(order);
		}

		void store(T desired, std::memory_order order = std::memory_order_seq_cst) const noexcept
		{
			reinterpret_cast<std::atomic<T>*>(m_target)->store(desired, order);
		}

	private:
		T* m_target;
	};
#endif
}
