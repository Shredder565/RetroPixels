// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "PPCState.h"

// Calling guest functions from host code.
//
// Arguments follow the PowerPC EABI as used by Cafe OS: integers and
// pointers go to r3-r10, floating point values to f1-f8 and integer
// arguments beyond the eighth to the caller's parameter area at sp+8.
// The return value is taken from r3.

// how many registers / stack slots the arguments placed so far use
struct PPCCoreCallbackData_t
{
	sint32 gprCount = 0;
	sint32 floatCount = 0;
	sint32 stackCount = 0;
};

inline void _PPCCoreCallback_writeGPRArg(PPCCoreCallbackData_t& data, PPCInterpreter_t* hCPU, uint32 value)
{
	if (data.gprCount >= 8)
	{
		// PPCCore_executeCallbackInternal lowers sp by 64 bytes before the
		// call, so the parameter area seen by the callee starts 64 bytes
		// below the current sp + 8
		constexpr sint32 kCallFrameBytes = 16 * 4;
		uint32 slotAddress = hCPU->gpr[1] + (uint32)(8 + data.stackCount * 4 - kCallFrameBytes);
		memory_writeU32(slotAddress, value);
		data.stackCount++;
		return;
	}
	hCPU->gpr[3 + data.gprCount] = value;
	data.gprCount++;
}

// all arguments are placed: run the function and return r3
inline uint32 PPCCoreCallback(MPTR function, const PPCCoreCallbackData_t& data)
{
	PPCInterpreter_t* cpu = PPCCore_executeCallbackInternal(function);
	return cpu->gpr[3];
}

// places one argument, then continues with the rest
template <typename T, typename... TArgs>
uint32 PPCCoreCallback(MPTR function, PPCCoreCallbackData_t& data, T currentArg, TArgs... args)
{
	cemu_assert_debug(data.floatCount < 8); // float arguments on the stack are not supported

	PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
	using TPlain = std::remove_reference_t<T>;
	if constexpr (std::is_enum_v<T>)
	{
		// pass enums as their underlying integer type
		using TUnderlying = std::underlying_type_t<T>;
		return PPCCoreCallback<TUnderlying>(function, data, static_cast<TUnderlying>(currentArg), std::forward<TArgs>(args)...);
	}
	else
	{
		if constexpr (std::is_pointer_v<T>)
			_PPCCoreCallback_writeGPRArg(data, hCPU, MEMPTR(currentArg).GetMPTR());
		else if constexpr (std::is_base_of_v<MEMPTRBase, TPlain>)
			_PPCCoreCallback_writeGPRArg(data, hCPU, currentArg.GetMPTR());
		else if constexpr (std::is_reference_v<T>)
			_PPCCoreCallback_writeGPRArg(data, hCPU, MEMPTR(&currentArg).GetMPTR());
		else if constexpr (std::is_floating_point_v<T>)
		{
			hCPU->fpr[1 + data.floatCount].fpr = (double)currentArg;
			data.floatCount++;
		}
		else if constexpr (std::is_integral_v<T> && sizeof(T) == 8)
		{
			// 64-bit integers take a register pair, high word first
			hCPU->gpr[3 + data.gprCount] = (uint32)((uint64)currentArg >> 32);
			hCPU->gpr[3 + data.gprCount + 1] = (uint32)currentArg;
			data.gprCount += 2;
		}
		else
			_PPCCoreCallback_writeGPRArg(data, hCPU, (uint32)currentArg);
		return PPCCoreCallback(function, data, args...);
	}
}

template <typename... TArgs>
uint32 PPCCoreCallback(MPTR function, TArgs... args)
{
	PPCCoreCallbackData_t data{};
	return PPCCoreCallback(function, data, std::forward<TArgs>(args)...);
}

// function given as a host pointer into guest memory
template <typename... TArgs>
uint32 PPCCoreCallback(void* functionPtr, TArgs... args)
{
	MEMPTR<void> function{ functionPtr };
	PPCCoreCallbackData_t data{};
	return PPCCoreCallback(function.GetMPTR(), data, std::forward<TArgs>(args)...);
}
