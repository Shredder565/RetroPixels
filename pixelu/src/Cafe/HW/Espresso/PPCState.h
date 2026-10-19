// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/MMU/MMU.h"

// Guest CPU state shared by the interpreter, the recompiler and the HLE
// libraries. The recompiler addresses PPCInterpreter_t fields by offset, so
// the member order below is part of the ABI between them.

// reasons a core can be pulled out of normal execution
enum
{
	CPUException_NOTHING,
	CPUException_FPUUNAVAILABLE,
	CPUException_EXTERNAL,
	CPUException_SYSTEMCALL
};

#define PPC_LWARX_RESERVATION_MAX	(4)

// A floating point register. The Espresso FPRs hold two values for paired
// single instructions (ps0/ps1); ps0 doubles as the regular FPR.
union FPR_t
{
	double fpr;
	struct
	{
		double fp0;
		double fp1;
	};
	struct
	{
		uint64 guint;
	};
	struct
	{
		uint64 fp0int;
		uint64 fp1int;
	};
};

// state shared by all three cores
struct PPCInterpreterGlobal_t
{
	struct
	{
		uint32 scr; // system control (core enable bits, ...)
		uint32 car;
	}sprGlobal;
	uint64 tb; // time base (LLE mode)
};

struct PPCInterpreter_t
{
	uint32 instructionPointer;
	uint32 gpr[32];
	FPR_t fpr[32];
	uint32 fpscr;
	// one byte per CR bit, each 0 or 1. Field n occupies cr[n*4 .. n*4+3]
	// in the order LT, GT, EQ, SO.
	uint8 cr[32];
	// XER flags kept outside of spr.XER for fast access
	uint8 xer_ca;
	uint8 xer_so;
	uint8 xer_ov;
	// time slice accounting: a thread switch is due once this drops below zero
	sint32 remainingCycles;
	// cycles given up by an early yield (restored when a callback returns)
	sint32 skippedCycles;
	// user mode SPRs
	struct
	{
		uint32 LR;
		uint32 CTR;
		uint32 XER;
		uint32 UPIR;
		uint32 UGQR[8];
	}spr;
	// lwarx reservation (stwcx. compares against the value read)
	uint32 reservedMemAddr;
	uint32 reservedMemValue;
	// scratch space for the recompiler backends
	FPR_t temporaryFPR[8];
	uint32 temporaryGPR[4]; // deprecated, still used by the x64 backend
	uint32 temporaryGPR_reg[4];
	// supervisor state, only used when running the kernel (LLE mode)
	struct
	{
		uint32 fpecr;
		uint32 DEC;
		uint32 srr0;
		uint32 srr1;
		uint32 PVR;
		uint32 msr;
		uint32 sprg[4];
		// data storage interrupt info
		uint32 dar;
		uint32 dsisr;
		// locked cache DMA
		uint32 dmaU;
		uint32 dmaL;
		// address translation
		uint32 dbatU[8];
		uint32 dbatL[8];
		uint32 ibatU[8];
		uint32 ibatL[8];
		uint32 sr[16];
		uint32 sdr1;
	}sprExtended;
	uint8 LSQE;
	uint8 PSE;
	PPCInterpreterGlobal_t* global;
	// set by the LLE memory model when an access raised a DSI
	bool memoryException;
	// interrupt mask of the core, see OSDisableInterrupts
	uint32 coreInterruptMask;
	// recompiler: host stack pointer saved on entry
	void* rspTemp;
};

// ---- HLE parameter access ----
// Arguments 0-7 are passed in r3-r10, further ones on the caller's stack
// starting at sp+8.

inline uint32 PPCInterpreter_getCallParamU32(PPCInterpreter_t* hCPU, uint32 index)
{
	if (index < 8)
		return hCPU->gpr[3 + index];
	return memory_readU32(hCPU->gpr[1] + 8 + (index - 8) * 4);
}

// 64-bit arguments occupy two consecutive slots, high word first
inline uint64 PPCInterpreter_getCallParamU64(PPCInterpreter_t* hCPU, uint32 index)
{
	uint64 high = PPCInterpreter_getCallParamU32(hCPU, index);
	uint64 low = PPCInterpreter_getCallParamU32(hCPU, index + 1);
	return (high << 32) | low;
}

#define ppcGetCallParamU32(__index) PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcGetCallParamU16(__index) ((uint16)(PPCInterpreter_getCallParamU32(hCPU, __index)&0xFFFF))
#define ppcGetCallParamU8(__index) ((uint8)(PPCInterpreter_getCallParamU32(hCPU, __index)&0xFF))
#define ppcGetCallParamStruct(__index, __type) ((__type*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))

#define ppcDefineParamU32(__name, __index) uint32 __name = PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcDefineParamU16(__name, __index) uint16 __name = (uint16)PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcDefineParamU32BEPtr(__name, __index) uint32be* __name = (uint32be*)((uint8*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamS32(__name, __index) sint32 __name = (sint32)PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcDefineParamU64(__name, __index) uint64 __name = PPCInterpreter_getCallParamU64(hCPU, __index)
#define ppcDefineParamMPTR(__name, __index) MPTR __name = (MPTR)PPCInterpreter_getCallParamU32(hCPU, __index)
#define ppcDefineParamMEMPTR(__name, __type, __index) MEMPTR<__type> __name{PPCInterpreter_getCallParamU32(hCPU, __index)}
#define ppcDefineParamU8(__name, __index) uint8 __name = (PPCInterpreter_getCallParamU32(hCPU, __index)&0xFF)
#define ppcDefineParamStructPtr(__name, __type, __index) __type* __name = ((__type*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamTypePtr(__name, __type, __index) __type* __name = ((__type*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamPtr(__name, __type, __index) __type* __name = ((__type*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamStr(__name, __index) char* __name = ((char*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamUStr(__name, __index) uint8* __name = ((uint8*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamWStr(__name, __index) wchar_t* __name = ((wchar_t*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))
#define ppcDefineParamWStrBE(__name, __index) uint16be* __name = ((uint16be*)memory_getPointerFromVirtualOffsetAllowNull(PPCInterpreter_getCallParamU32(hCPU, __index)))

// stack pointer register
#define GPR_SP 1

// ---- interpreter ----

PPCInterpreter_t* PPCInterpreter_createInstance(unsigned int Entrypoint);
PPCInterpreter_t* PPCInterpreter_getCurrentInstance();
void PPCInterpreter_setCurrentInstance(PPCInterpreter_t* hCPU);

uint64 PPCInterpreter_getMainCoreCycleCounter();

void PPCInterpreter_nextInstruction(PPCInterpreter_t* cpuInterpreter);
void PPCInterpreter_jumpToInstruction(PPCInterpreter_t* cpuInterpreter, uint32 newIP);

// HLE mode (user mode code, flat memory)
void PPCInterpreterSlim_executeInstruction(PPCInterpreter_t* hCPU);
// LLE mode (kernel code, address translation)
void PPCInterpreterFull_executeInstruction(PPCInterpreter_t* hCPU);

uint32 PPCInterpreter_getXER(PPCInterpreter_t* hCPU);
void PPCInterpreter_setXER(PPCInterpreter_t* hCPU, uint32 v);

// clock rates, kept for older code (see Espresso/Const.h)
#define ESPRESSO_CORE_CLOCK       1243125000
#define ESPRESSO_BUS_CLOCK        248625000
#define ESPRESSO_TIMER_CLOCK      (ESPRESSO_BUS_CLOCK/4)

// the timer clock is 1/20 of the core clock
#define ESPRESSO_CORE_CLOCK_TO_TIMER_CLOCK(__cc) ((__cc)/20ULL)

// exception vector offsets
#define CPU_EXCEPTION_DSI			0x00000300
#define CPU_EXCEPTION_INTERRUPT		0x00000500
#define CPU_EXCEPTION_FPUUNAVAIL	0x00000800
#define CPU_EXCEPTION_SYSTEMCALL	0x00000C00
#define CPU_EXCEPTION_DECREMENTER	0x00000900

// Guest code always runs with MSR[FP] set, so FP availability is not checked
#define FPUCheckAvailable()

void PPCSpr_set(PPCInterpreter_t* hCPU, uint32 spr, uint32 newValue);
uint32 PPCSpr_get(PPCInterpreter_t* hCPU, uint32 spr);

uint32 PPCInterpreter_getCoreIndex(PPCInterpreter_t* hCPU);
uint32 PPCInterpreter_getCurrentCoreIndex();

void PPCInterpreter_setDEC(PPCInterpreter_t* hCPU, uint32 newValue);

// ---- guest time ----

extern uint64 ppcCyclesSince2000;           // core cycles since 2000-01-01, set at start-up
extern uint64 ppcCyclesSince2000TimerClock; // the same in timer ticks
extern uint64 ppcCyclesSince2000_UTC;
extern uint64 ppcMainThreadDECCycleValue;   // last value written to DEC
extern uint64 ppcMainThreadDECCycleStart;   // cycle counter at that write

// host TSC based guest clock
void PPCTimer_init();
void PPCTimer_waitForInit();
uint64 PPCTimer_getFromRDTSC();

uint64 PPCTimer_microsecondsToTsc(uint64 us);
uint64 PPCTimer_tscToMicroseconds(uint64 us);
uint64 PPCTimer_getRawTsc();

void PPCTimer_start();

// ---- cores ----

// instructions a guest thread runs before the scheduler is invoked
extern uint32 ppcThreadQuantum;

uint8* PPCInterpreter_PushAndReturnStackPointer(sint32 offset);
uint8* PPCInterpreterGetStackPointer();
void PPCInterpreterModifyStackPointer(sint32 offset);

uint32 PPCInterpreter_makeCallableExportDepr(void (*ppcCallableExport)(PPCInterpreter_t* hCPU));

// denormals are kept as they are
inline float flushDenormalToZero(float f)
{
	return f;
}

// ---- HLE functions (primary opcode 1) ----

using HLECALL = void(*)(PPCInterpreter_t*);
using HLEIDX = sint32;

HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName);
HLECALL PPCInterpreter_getHLECall(HLEIDX funcIndex);

// ---- HLE scheduling ----

void PPCInterpreter_relinquishTimeslice();

void PPCCore_boostQuantum(sint32 numCycles);
void PPCCore_deboostQuantum(sint32 numCycles);

void PPCCore_switchToScheduler();
void PPCCore_switchToSchedulerWithLock();

// runs guest code at functionMPTR on the current thread until it returns
PPCInterpreter_t* PPCCore_executeCallbackInternal(uint32 functionMPTR);
void PPCCore_init();

// ---- LLE scheduling ----

void PPCCoreLLE_startSingleCoreScheduler(uint32 entrypoint);
