// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Time slicing for guest threads in HLE mode. Each host thread that runs
// guest code owns a PPCInterpreter_t; remainingCycles counts down the
// current time slice and coreinit's scheduler takes over once it runs out.

#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/CafeSystem.h"

// default time slice in instructions, game profiles may change it
uint32 ppcThreadQuantum = 45000;

namespace coreinit
{
	void __OSThreadSwitchToNext();
}

namespace
{
	// one title is known to switch threads with interrupts enabled
	constexpr uint64 kTitleSwitchingWithInterruptsOn = 0x000500001019e600;

	bool SchedulerEntryAllowed(PPCInterpreter_t* cpu)
	{
		return cpu->coreInterruptMask != 0 || CafeSystem::GetForegroundTitleId() == kTitleSwitchingWithInterruptsOn;
	}

	// return address used for host initiated calls into guest code: ends the
	// time slice and signals completion with an instruction pointer of 0
	void CallbackReturnStub(PPCInterpreter_t* cpu)
	{
		PPCInterpreter_relinquishTimeslice();
		cpu->instructionPointer = 0;
	}

	// size of the frame reserved below the caller's stack pointer
	constexpr uint32 kCallbackFrameSize = 64;
}

// ends the current time slice early, remembering how much of it was left
void PPCInterpreter_relinquishTimeslice()
{
	PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
	if (cpu->remainingCycles < 0)
		return;
	cpu->skippedCycles = cpu->remainingCycles + 1;
	cpu->remainingCycles = -1;
}

void PPCCore_boostQuantum(sint32 numCycles)
{
	PPCInterpreter_getCurrentInstance()->remainingCycles += numCycles;
}

void PPCCore_deboostQuantum(sint32 numCycles)
{
	PPCInterpreter_getCurrentInstance()->remainingCycles -= numCycles;
}

void PPCCore_switchToScheduler()
{
	cemu_assert_debug(!__OSHasSchedulerLock()); // the lock must not be held across a time slice
	cemu_assert_debug(SchedulerEntryAllowed(PPCInterpreter_getCurrentInstance()));
	__OSLockScheduler();
	coreinit::__OSThreadSwitchToNext();
	__OSUnlockScheduler();
}

void PPCCore_switchToSchedulerWithLock()
{
	cemu_assert_debug(__OSHasSchedulerLock());
	cemu_assert_debug(SchedulerEntryAllowed(PPCInterpreter_getCurrentInstance()));
	coreinit::__OSThreadSwitchToNext();
}

PPCInterpreter_t* PPCCore_executeCallbackInternal(uint32 functionMPTR)
{
	cemu_assert_debug(functionMPTR != 0);
	PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();

	const uint32 savedLR = cpu->spr.LR;
	const uint32 savedIP = cpu->instructionPointer;
	cpu->gpr[1] -= kCallbackFrameSize;
	cpu->spr.LR = PPCInterpreter_makeCallableExportDepr(CallbackReturnStub);
	cpu->instructionPointer = functionMPTR;

	for (;;)
	{
		cpu->remainingCycles = ppcThreadQuantum;
		cpu->skippedCycles = 0;
		if (cpu->remainingCycles > 0)
		{
			// prefer recompiled code, then finish the slice in the interpreter
			PPCRecompiler_attemptEnter(cpu, cpu->instructionPointer);
			while (--cpu->remainingCycles >= 0)
				PPCInterpreterSlim_executeInstruction(cpu);
		}
		if (cpu->instructionPointer == 0)
		{
			// the function returned: give back the unused part of the slice
			cpu->remainingCycles += cpu->skippedCycles;
			cpu->skippedCycles = 0;
			break;
		}
		// slice used up while still inside the function
		coreinit::OSYieldThread();
	}

	cpu->gpr[1] += kCallbackFrameSize;
	cpu->spr.LR = savedLR;
	cpu->instructionPointer = savedIP;
	return cpu;
}

void PPCCore_init()
{
}
