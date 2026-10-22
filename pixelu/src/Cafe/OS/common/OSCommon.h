// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Common declarations for the HLE system libraries: the export registry, the
// coreinit headers, the C++ <-> PowerPC call glue and PowerPC varargs.
#pragma once

struct PPCInterpreter_t;

// export registry (OSCommon.cpp)
void osLib_generateHashFromName(const char* name, uint32* hashA, uint32* hashB);
sint32 osLib_getFunctionIndex(const char* libraryName, const char* functionName); // -1 if not registered
uint32 osLib_getPointer(const char* libraryName, const char* functionName); // 0xFFFFFFFF if not registered

void osLib_addFunctionInternal(const char* libraryName, const char* functionName, void(*osFunction)(PPCInterpreter_t* hCPU));
#define osLib_addFunction(__p1, __p2, __p3) osLib_addFunctionInternal((const char*)__p1, __p2, __p3)
void osLib_addVirtualPointer(const char* libraryName, const char* functionName, uint32 vPtr);

// for handlers that take PPCInterpreter_t* directly: set r3 (r3:r4) and return to LR
void osLib_returnFromFunction(PPCInterpreter_t* hCPU, uint32 returnValue);
void osLib_returnFromFunction64(PPCInterpreter_t* hCPU, uint64 returnValue64);

#include "Cafe/OS/libs/coreinit/coreinit.h"

namespace coreinit
{
	// reason passed to a module's entry point (__rpl_start / rpl_entry)
	enum class RplEntryReason
	{
		Loaded = 1,
		Unloaded = 2,
	};
}

#include "Cafe/OS/common/OSUtil.h"

// PowerPC EABI va_list
struct ppc_va_list
{
	uint8be gprIndex; // next r3..r10 slot (0-8)
	uint8be fprIndex; // next f1..f8 slot (0-8)
	uint8be _padding2[2];
	MEMPTR<uint8be> overflow_arg_area; // arguments passed on the stack
	MEMPTR<uint8be> reg_save_area; // spilled r3..r10 followed by f1..f8
};
static_assert(sizeof(ppc_va_list) == 0xC);

struct ppc_va_list_reg_storage
{
	uint32be gpr_save_area[8]; // r3..r10
	float64be fpr_save_area[8]; // f1..f8
	ppc_va_list vargs;
	uint32be padding;
};
static_assert(sizeof(ppc_va_list_reg_storage) == 0x70);
static_assert(offsetof(ppc_va_list_reg_storage, fpr_save_area) == 0x20);

// fill a register save area from the current CPU state, like the prologue of a variadic guest function
inline ppc_va_list& _ppc_va_setup(ppc_va_list_reg_storage* store, MPTR callerStack, uint8 gprUsed, uint8 fprUsed)
{
	PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
	for (int r = 0; r < 8; r++)
		store->gpr_save_area[r] = cpu->gpr[3 + r];
	for (int f = 0; f < 8; f++)
		store->fpr_save_area[f] = cpu->fpr[1 + f].fp0;
	store->vargs.gprIndex = gprUsed;
	store->vargs.fprIndex = fprUsed;
	store->vargs.reg_save_area = (uint8be*)store;
	store->vargs.overflow_arg_area = MEMPTR<uint8be>(callerStack + 8);
	return store->vargs;
}

// va_start for HLE functions: declares 'vargs' after the named arguments
// (__gprIndex integer and __fprIndex float registers). The caller's stack
// pointer is read before the save area is pushed. Use it before any other
// StackAllocator in the function.
#define ppc_define_va_list(__gprIndex, __fprIndex) \
	const MPTR _ppcVaCallerSp = PPCInterpreter_getCurrentInstance()->gpr[1]; \
	StackAllocator<ppc_va_list_reg_storage> _ppcVaStore; \
	ppc_va_list& vargs = _ppc_va_setup(_ppcVaStore.GetPointer(), _ppcVaCallerSp, (uint8)(__gprIndex), (uint8)(__fprIndex));

enum class ppc_va_type
{
	INT32 = 1,
	INT64 = 2,
	FLOAT_OR_DOUBLE = 3,
};

// va_arg: returns a host pointer to the (big endian) value of the next argument
static void* _ppc_va_arg(ppc_va_list* vargs, ppc_va_type argType)
{
	auto fromStack = [vargs](uint32 size) -> void* {
		MPTR at = vargs->overflow_arg_area.GetMPTR();
		if (size == 8)
			at = (at + 7) & ~7u;
		vargs->overflow_arg_area = MEMPTR<uint8be>(at + size);
		return memory_getPointerFromVirtualOffset(at);
	};
	uint8be* regs = vargs->reg_save_area.GetPtr();
	if (argType == ppc_va_type::FLOAT_OR_DOUBLE)
	{
		const uint8 n = vargs->fprIndex;
		if (n < 8)
		{
			vargs->fprIndex = (uint8)(n + 1);
			return regs + 0x20 + n * 8;
		}
		return fromStack(8);
	}
	if (argType == ppc_va_type::INT64)
	{
		uint8 n = vargs->gprIndex;
		n = (uint8)(n + (n & 1)); // register pairs start on an odd register (r3, r5, r7, r9)
		vargs->gprIndex = n;
		if (n < 8)
		{
			vargs->gprIndex = (uint8)(n + 2);
			return regs + n * 4;
		}
		return fromStack(8);
	}
	if (argType != ppc_va_type::INT32)
		cemu_assert_suspicious(); // treated as a 32-bit integer
	const uint8 n = vargs->gprIndex;
	if (n < 8)
	{
		vargs->gprIndex = (uint8)(n + 1);
		return regs + n * 4;
	}
	return fromStack(4);
}
