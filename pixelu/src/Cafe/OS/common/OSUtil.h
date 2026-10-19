// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Glue between host C++ functions and the PowerPC calling convention used by
// Wii U software. An HLE export is an ordinary C++ function; the templates
// here pull its arguments out of the guest registers/stack (EABI: integers
// and pointers in r3..r10, then on the caller's stack at r1+8, 64-bit values
// in aligned register pairs, floating point in f1..f8), call it, store the
// result in r3 (and r4 for 64-bit values) and return to the caller's LR.
#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/MMU/MMU.h"

#include <fmt/ostream.h>
#include <fmt/compile.h>
#include <fmt/ranges.h>

namespace ppcabi
{
	// walks the argument list of one call
	struct ArgCursor
	{
		PPCInterpreter_t* cpu;
		int gpr = 0; // number of 32-bit integer slots consumed
		int fpr = 0; // number of floating point registers consumed

		explicit ArgCursor(PPCInterpreter_t* c) : cpu(c) {}

		// stack slot n (n >= 8) lives in the caller's parameter area
		MPTR StackSlot(int n) const
		{
			return cpu->gpr[1] + 8 + (n - 8) * 4;
		}

		uint32 Next32()
		{
			const int n = gpr++;
			if (n < 8)
				return cpu->gpr[3 + n];
			return memory_readU32(StackSlot(n));
		}

		uint64 Next64()
		{
			// 64-bit values start on an even slot
			if (gpr & 1)
				gpr++;
			const int n = gpr;
			gpr += 2;
			if (n < 8)
				return ((uint64)cpu->gpr[3 + n] << 32) | (uint64)cpu->gpr[3 + n + 1];
			return memory_readU64(StackSlot(n));
		}

		double NextFloat()
		{
			return cpu->fpr[1 + fpr++].fpr;
		}
	};

	template<typename T>
	inline void Fetch(ArgCursor& cur, T& out)
	{
		if constexpr (std::is_pointer_v<T>)
		{
			out = MEMPTR<std::remove_pointer_t<T>>(cur.Next32()).GetPtr();
		}
		else if constexpr (std::is_base_of_v<MEMPTRBase, T>)
		{
			out = cur.Next32();
		}
		else if constexpr (std::is_enum_v<T>)
		{
			std::underlying_type_t<T> raw;
			Fetch(cur, raw);
			out = (T)raw;
		}
		else if constexpr (std::is_integral_v<T>)
		{
			if constexpr (sizeof(T) == 8)
				out = (T)cur.Next64();
			else
				out = (T)cur.Next32();
		}
		else if constexpr (std::is_floating_point_v<T>)
		{
			out = (T)cur.NextFloat();
		}
		else
		{
			// no mapping for this parameter type
			DEBUG_BREAK;
		}
	}

	template<typename T>
	inline T Take(ArgCursor& cur)
	{
		T v;
		Fetch(cur, v);
		return v;
	}

	// reads all arguments of fn in order (braced init guarantees left-to-right)
	template<typename R, typename... Args>
	inline std::tuple<Args...> TakeAll(PPCInterpreter_t* cpu, R(*)(Args...))
	{
		ArgCursor cur(cpu);
		return std::tuple<Args...>{ Take<Args>(cur)... };
	}

	template<typename T>
	inline void StoreResult(PPCInterpreter_t* cpu, T r)
	{
		if constexpr (std::is_pointer_v<T>)
			cpu->gpr[3] = MEMPTR(r).GetMPTR();
		else if constexpr (std::is_reference_v<T>)
			cpu->gpr[3] = MEMPTR(&r).GetMPTR();
		else if constexpr (std::is_enum_v<T>)
			StoreResult<std::underlying_type_t<T>>(cpu, (std::underlying_type_t<T>)r);
		else if constexpr (std::is_integral_v<T> && sizeof(T) == 8)
		{
			const uint64 v = (uint64)r;
			cpu->gpr[3] = (uint32)(v >> 32);
			cpu->gpr[4] = (uint32)v;
		}
		else if constexpr (std::is_integral_v<T>)
			cpu->gpr[3] = (uint32)r;
		else
			cemu_assert_unimplemented(); // registers are left untouched
	}

	// --- logging helpers ---

	// a guest pointer shown in the log as its guest address
	struct LogAddr
	{
		uint32 addr;
	};

	template<typename T>
	struct LogArgType
	{
		using type = T;
	};
	template<typename T>
		requires std::is_pointer_v<T> && (!std::is_same_v<T, char*>) && (!std::is_same_v<T, const char*>)
	struct LogArgType<T>
	{
		using type = LogAddr;
	};

	template<typename T>
	inline auto TakeForLog(ArgCursor& cur)
	{
		if constexpr (std::is_same_v<typename LogArgType<T>::type, LogAddr>)
			return LogAddr{ cur.Next32() };
		else
		{
			T v = Take<T>(cur);
			if constexpr (std::is_same_v<T, char*> || std::is_same_v<T, const char*>)
			{
				if (!v)
					v = (T)"null"; // fmt refuses null strings
			}
			return v;
		}
	}

	template<typename R, typename... Args>
	inline auto TakeAllForLog(PPCInterpreter_t* cpu, R(*)(Args...))
	{
		ArgCursor cur(cpu);
		return std::tuple<typename LogArgType<Args>::type...>{ TakeForLog<Args>(cur)... };
	}

	template<typename T>
	inline auto ResultForLog(T r)
	{
		if constexpr (std::is_pointer_v<T>)
			return MEMPTR(r).GetMPTR();
		else if constexpr (std::is_enum_v<T>)
			return static_cast<std::underlying_type_t<T>>(r);
		else if constexpr (!std::is_fundamental_v<T>)
			return MEMPTR(&r).GetMPTR();
		else
			return r;
	}

	// keepRef: a function returning a reference hands back the address of the referenced object
	template<auto fn, bool keepRef>
	inline void InvokeAndReturn(PPCInterpreter_t* cpu, auto&& args, auto&& onResult)
	{
		using Ret = decltype(std::apply(fn, args));
		if constexpr (std::is_void_v<Ret>)
			std::apply(fn, args);
		else
		{
			decltype(auto) r = std::apply(fn, args);
			if constexpr (keepRef)
				StoreResult<Ret>(cpu, r);
			else
				StoreResult<std::remove_cvref_t<Ret>>(cpu, r);
			onResult(r);
		}
		cpu->instructionPointer = cpu->spr.LR;
	}
}

template<>
struct fmt::formatter<ppcabi::LogAddr> : formatter<string_view>
{
	template<typename FormatContext>
	auto format(const ppcabi::LogAddr& v, FormatContext& ctx) const -> format_context::iterator
	{
		return fmt::format_to(ctx.out(), "{:#x}", v.addr);
	}
};

// HLE entry point generated for each registered export
template<auto fn, typename TNames, LogType TLogType>
void cafeExportCallWrapper(PPCInterpreter_t* hCPU)
{
	auto args = ppcabi::TakeAll(hCPU, fn);
	bool logged = false;
	if (cemuLog_isLoggingEnabled(TLogType))
	{
		const auto shown = ppcabi::TakeAllForLog(hCPU, fn);
		constexpr bool hasArgs = std::tuple_size_v<std::remove_cvref_t<decltype(shown)>> > 0;
		if (cemuLog_advancedPPCLoggingEnabled())
		{
			MPTR thread = memory_getVirtualOffsetFromPointer(coreinit::OSGetCurrentThread());
			if constexpr (hasArgs)
				logged = cemuLog_log(TLogType, "{}.{}{} # LR: {:#x} | Thread: {:#x}", TNames::GetLib(), TNames::GetFunc(), shown, hCPU->spr.LR, thread);
			else
				logged = cemuLog_log(TLogType, "{}.{}() # LR: {:#x} | Thread: {:#x}", TNames::GetLib(), TNames::GetFunc(), hCPU->spr.LR, thread);
		}
		else
		{
			if constexpr (hasArgs)
				logged = cemuLog_log(TLogType, "{}.{}{}", TNames::GetLib(), TNames::GetFunc(), shown);
			else
				logged = cemuLog_log(TLogType, "{}.{}()", TNames::GetLib(), TNames::GetFunc());
		}
	}
	ppcabi::InvokeAndReturn<fn, true>(hCPU, args, [&](auto&& r) {
		if (logged)
			cemuLog_log(TLogType, "\t\t{}.{} -> {}", TNames::GetLib(), TNames::GetFunc(), ppcabi::ResultForLog(r));
	});
}

void osLib_addFunctionInternal(const char* libraryName, const char* functionName, void(*osFunction)(PPCInterpreter_t* hCPU));

template<auto fn, typename TNames, LogType TLogType>
void cafeExportMakeWrapper(const char* libname, const char* funcname)
{
	osLib_addFunctionInternal(libname, funcname, &cafeExportCallWrapper<fn, TNames, TLogType>);
}

// register a C++ function as export <libname>.<function name>
#define cafeExportRegister(__libname, __func, __logtype) \
	{ \
		struct StringWrapper { \
			static const char* GetLib() { return __libname; }; \
			static const char* GetFunc() { return #__func; }; \
		}; \
		cafeExportMakeWrapper<__func, StringWrapper, __logtype>(__libname, #__func); \
	}

// register a C++ function under an explicit export name
#define cafeExportRegisterFunc(__func, __libname, __funcname, __logtype) \
	{ \
		struct StringWrapper { \
			static const char* GetLib() { return __libname; }; \
			static const char* GetFunc() { return __funcname; }; \
		}; \
		cafeExportMakeWrapper<__func, StringWrapper, __logtype>(__libname, __funcname); \
	}

// a host function guest code can call through a function pointer (no export name, no logging)
template<auto fn>
void cafeCallableExportWrapper(PPCInterpreter_t* hCPU)
{
	auto args = ppcabi::TakeAll(hCPU, fn);
	ppcabi::InvokeAndReturn<fn, false>(hCPU, args, [](auto&&) {});
}

template<auto fn>
MPTR makeCallableExport()
{
	return RPLLoader_MakePPCCallable(&cafeCallableExportWrapper<fn>);
}

void osLib_addVirtualPointer(const char* libraryName, const char* functionName, uint32 vPtr);
