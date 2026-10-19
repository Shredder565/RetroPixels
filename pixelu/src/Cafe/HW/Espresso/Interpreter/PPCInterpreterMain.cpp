// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// CPU instance management and small accessors around PPCInterpreter_t.

#include "PPCInterpreterInternal.h"

// Timing state shared with coreinit's time functions
uint64 ppcCyclesSince2000 = 0;
uint64 ppcCyclesSince2000TimerClock = 0;
uint64 ppcCyclesSince2000_UTC = 0;
uint64 ppcMainThreadDECCycleValue = 0;
uint64 ppcMainThreadDECCycleStart = 0;

namespace
{
	// the CPU context the current host thread is executing
	thread_local PPCInterpreter_t* t_activeCPU = nullptr;

	// The recompiler uses memory right below the context as scratch stack
	// (exception handler path), so every instance gets a reserved area in
	// front of it.
	constexpr size_t kContextScratchBytes = 0x6000;
}

PPCInterpreter_t* PPCInterpreter_createInstance(unsigned int Entrypoint)
{
	uint8* block = (uint8*)malloc(kContextScratchBytes + sizeof(PPCInterpreter_t));
	PPCInterpreter_t* cpu = (PPCInterpreter_t*)(block + kContextScratchBytes);
	memset((void*)cpu, 0, sizeof(PPCInterpreter_t));
	cpu->instructionPointer = Entrypoint;
	return cpu;
}

TLS_WORKAROUND_NOINLINE PPCInterpreter_t* PPCInterpreter_getCurrentInstance()
{
	return t_activeCPU;
}

TLS_WORKAROUND_NOINLINE void PPCInterpreter_setCurrentInstance(PPCInterpreter_t* hCPU)
{
	t_activeCPU = hCPU;
}

uint64 PPCInterpreter_getMainCoreCycleCounter()
{
	return PPCTimer_getFromRDTSC();
}

void PPCInterpreter_nextInstruction(PPCInterpreter_t* cpuInterpreter)
{
	cpuInterpreter->instructionPointer += 4;
}

void PPCInterpreter_jumpToInstruction(PPCInterpreter_t* cpuInterpreter, uint32 newIP)
{
	cpuInterpreter->instructionPointer = newIP;
}

void PPCInterpreter_setDEC(PPCInterpreter_t* hCPU, uint32 newValue)
{
	hCPU->sprExtended.DEC = newValue;
	ppcMainThreadDECCycleStart = PPCInterpreter_getMainCoreCycleCounter();
	ppcMainThreadDECCycleValue = newValue;
}

// XER as seen by mfxer: CA/OV/SO come from their separate fields
uint32 PPCInterpreter_getXER(PPCInterpreter_t* hCPU)
{
	constexpr uint32 kFlagBits = (1u << XER_BIT_CA) | (1u << XER_BIT_OV) | (1u << XER_BIT_SO);
	uint32 value = hCPU->spr.XER & ~kFlagBits;
	value |= (uint32)(hCPU->xer_ca ? 1 : 0) << XER_BIT_CA;
	value |= (uint32)(hCPU->xer_ov ? 1 : 0) << XER_BIT_OV;
	value |= (uint32)(hCPU->xer_so ? 1 : 0) << XER_BIT_SO;
	return value;
}

// bits 24-28 of XER are reserved and read as zero
void PPCInterpreter_setXER(PPCInterpreter_t* hCPU, uint32 v)
{
	hCPU->spr.XER = v & 0xE0FFFFFF;
	hCPU->xer_ca = (uint8)((v >> XER_BIT_CA) & 1);
	hCPU->xer_ov = (uint8)((v >> XER_BIT_OV) & 1);
	hCPU->xer_so = (uint8)((v >> XER_BIT_SO) & 1);
}

uint32 PPCInterpreter_getCoreIndex(PPCInterpreter_t* hCPU)
{
	return hCPU->spr.UPIR;
}

uint32 PPCInterpreter_getCurrentCoreIndex()
{
	return PPCInterpreter_getCurrentInstance()->spr.UPIR;
}

uint8* PPCInterpreterGetStackPointer()
{
	return memory_getPointerFromVirtualOffset(PPCInterpreter_getCurrentInstance()->gpr[1]);
}

// moves the guest stack pointer down by offset bytes and returns the new top
uint8* PPCInterpreter_PushAndReturnStackPointer(sint32 offset)
{
	PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
	cpu->gpr[1] -= offset;
	return memory_getPointerFromVirtualOffset(cpu->gpr[1]);
}

void PPCInterpreterModifyStackPointer(sint32 offset)
{
	PPCInterpreter_getCurrentInstance()->gpr[1] -= offset;
}

uint32 RPLLoader_MakePPCCallable(void (*ppcCallableExport)(PPCInterpreter_t* hCPU));

// kept for older callers, forwards to the RPL loader
uint32 PPCInterpreter_makeCallableExportDepr(void (*ppcCallableExport)(PPCInterpreter_t* hCPU))
{
	return RPLLoader_MakePPCCallable(ppcCallableExport);
}
