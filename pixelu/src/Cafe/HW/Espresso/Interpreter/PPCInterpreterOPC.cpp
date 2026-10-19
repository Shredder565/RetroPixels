// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Branches, condition register moves, cache control, traps and the few
// supervisor instructions the interpreter knows about. None of these touch
// guest memory directly, so they are shared by both memory models.

#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"

namespace EspressoItp
{
	// ---- branches ----

	// evaluates the BO/BI condition of bc/bclr; decrements CTR when BO says so
	static bool EvaluateBranchCondition(PPCInterpreter_t* cpu, uint32 bo, uint32 bi)
	{
		const bool useCtr = (bo & 4) == 0;
		if (useCtr)
			cpu->spr.CTR--;
		const bool ctrOk = !useCtr || ((cpu->spr.CTR != 0) != ((bo & 2) != 0));
		const bool condOk = (bo & 16) != 0 || ((cpu->cr[bi] != 0) == ((bo & 8) != 0));
		return ctrOk && condOk;
	}

	void Op_B(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 target;
		PPC_OPC_TEMPL_I(op, target);
		if ((op & PPC_OPC_AA) == 0)
			target += cpu->instructionPointer;
		if (op & PPC_OPC_LK)
		{
			cpu->spr.LR = cpu->instructionPointer + 4;
			cpu->instructionPointer = target;
			// calls are a good point to switch over to recompiled code
			PPCRecompiler_attemptEnter(cpu, target);
			return;
		}
		cpu->instructionPointer = target;
	}

	void Op_BC(PPCInterpreter_t* cpu, uint32 op)
	{
		if (!EvaluateBranchCondition(cpu, ppcOpField_D(op), ppcOpField_A(op)))
		{
			PPCInterpreter_nextInstruction(cpu);
			return;
		}
		uint32 target = ppcOpSImm16(op & 0xFFFC);
		if ((op & PPC_OPC_AA) == 0)
			target += cpu->instructionPointer;
		else
			cemu_assert_unimplemented(); // absolute conditional branches are not expected in Wii U code
		if (op & PPC_OPC_LK)
			cpu->spr.LR = cpu->instructionPointer + 4;
		cpu->instructionPointer = target;
	}

	void Op_BCLR(PPCInterpreter_t* cpu, uint32 op)
	{
		if ((ppcOpField_D(op) & 4) == 0 && cpu->spr.CTR == 0)
			cemuLog_logDebug(LogType::Force, "bclr: CTR wraps around at {:08x}", cpu->instructionPointer);
		if (!EvaluateBranchCondition(cpu, ppcOpField_D(op), ppcOpField_A(op)))
		{
			PPCInterpreter_nextInstruction(cpu);
			return;
		}
		uint32 target = cpu->spr.LR & ~3u;
		if (op & PPC_OPC_LK)
			cpu->spr.LR = cpu->instructionPointer + 4;
		cpu->instructionPointer = target;
		PPCRecompiler_attemptEnter(cpu, target);
	}

	// bcctr only tests the CR bit (CTR decrementing forms are invalid)
	void Op_BCCTR(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 bo = ppcOpField_D(op);
		bool condOk = (bo & 16) != 0 || ((cpu->cr[ppcOpField_A(op)] != 0) == ((bo & 8) != 0));
		if (!condOk)
		{
			cpu->instructionPointer += 4;
			return;
		}
		if (op & PPC_OPC_LK)
			cpu->spr.LR = cpu->instructionPointer + 4;
		cpu->instructionPointer = cpu->spr.CTR & ~3u;
		PPCRecompiler_attemptEnter(cpu, cpu->instructionPointer);
	}

	// ---- system ----

	void Op_SC(PPCInterpreter_t* cpu, uint32 op)
	{
		cemuLog_logDebug(LogType::Force, "Interpreter: sc at {:08x} ignored", cpu->instructionPointer);
		PPCInterpreter_nextInstruction(cpu);
	}

	// return from interrupt: restores the MSR bits saved in SRR1 and jumps to SRR0
	void Op_RFI(PPCInterpreter_t* cpu, uint32 op)
	{
		constexpr uint32 kRestoredBits = 0x87C0FF73;
		cemuLog_logDebug(LogType::Force, "Interpreter: rfi to {:08x}", cpu->sprExtended.srr0);
		uint32 msr = cpu->sprExtended.msr & ~(kRestoredBits | MSR_POW);
		msr |= cpu->sprExtended.srr1 & kRestoredBits;
		cpu->sprExtended.msr = msr | MSR_RI;
		cpu->instructionPointer = cpu->sprExtended.srr0;
	}

	// mfmsr/mtmsr in problem state would raise a program exception; the
	// instruction just does not complete (and is retried)
	void Op_MFMSR(PPCInterpreter_t* cpu, uint32 op)
	{
		cemuLog_logDebug(LogType::Force, "Interpreter: mfmsr at {:08x}", cpu->instructionPointer);
		if (cpu->sprExtended.msr & MSR_PR)
			return;
		cpu->gpr[ppcOpField_D(op)] = cpu->sprExtended.msr;
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_MTMSR(PPCInterpreter_t* cpu, uint32 op)
	{
		cemuLog_logDebug(LogType::Force, "Interpreter: mtmsr at {:08x}", cpu->instructionPointer);
		if (cpu->sprExtended.msr & MSR_PR)
			return;
		cpu->sprExtended.msr = cpu->gpr[ppcOpField_D(op)];
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- condition register moves ----

	void Op_MCRF(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 dst = (ppcOpField_D(op) >> 2) * 4;
		uint32 src = (ppcOpField_A(op) >> 2) * 4;
		uint8 field[4];
		std::copy_n(cpu->cr + src, 4, field);
		std::copy_n(field, 4, cpu->cr + dst);
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_MFCR(PPCInterpreter_t* cpu, uint32 op)
	{
		cpu->gpr[ppcOpField_D(op)] = ppc_getCR(cpu);
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_MTCRF(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 bitMask = ppc_MTCRFMaskToCRBitMask((op >> 12) & 0xFF);
		uint32 value = cpu->gpr[ppcOpField_D(op)];
		for (uint32 bit = 0; bit < 32; bit++)
		{
			if ((bitMask >> bit) & 1)
				cpu->cr[bit] = (uint8)((value >> (31 - bit)) & 1);
		}
		PPCInterpreter_nextInstruction(cpu);
	}

	// mcrxr crfD: CR field crfD = XER[0-3] (SO, OV, CA, 0 -> LT, GT, EQ, SO), then XER[0-3] = 0
	void Op_MCRXR(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 crBase = ((op >> 23) & 7) * 4;
		uint32 xer = PPCInterpreter_getXER(cpu);
		for (uint32 i = 0; i < 4; i++)
			cpu->cr[crBase + i] = (uint8)((xer >> (31 - i)) & 1);
		PPCInterpreter_setXER(cpu, xer & 0x0FFFFFFF);
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- cache control ----

	static uint32 IndexedEA(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 rA = ppcOpField_A(op);
		return (rA ? cpu->gpr[rA] : 0) + cpu->gpr[ppcOpField_B(op)];
	}

	// dcbst/dcbf: the GPU buffer cache wants to know when the CPU writes back a line
	void Op_CacheFlush(PPCInterpreter_t* cpu, uint32 op)
	{
		LatteBufferCache_notifyDCFlush(IndexedEA(cpu, op), 32);
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_ICBI(PPCInterpreter_t* cpu, uint32 op)
	{
		coreinit::codeGenHandleICBI(IndexedEA(cpu, op));
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_NoEffect(PPCInterpreter_t* cpu, uint32 op)
	{
		PPCInterpreter_nextInstruction(cpu);
	}

	void Op_TLBSYNC(PPCInterpreter_t* cpu, uint32 op)
	{
		cemu_assert_unimplemented();
		PPCInterpreter_nextInstruction(cpu);
	}

	// ---- traps ----

	// Only the unconditional "tw 0, rX, rY" forms planted by the debuggers
	// are used; rA tells which debugger owns the breakpoint. The debugger
	// moves the instruction pointer itself.
	void Op_TW(PPCInterpreter_t* cpu, uint32 op)
	{
		uint32 to = ppcOpField_D(op);
		uint32 rA = ppcOpField_A(op);
		cemu_assert_debug(to == 0);
		if (to != 0)
			PPCInterpreter_nextInstruction(cpu);
		if (rA == DEBUGGER_BP_T_DEBUGGER)
			debugger_enterTW(cpu);
		else if (rA == DEBUGGER_BP_T_GDBSTUB)
			g_gdbstub->HandleTrapInstruction(cpu);
	}

	void Op_TWI(PPCInterpreter_t* cpu, uint32 op)
	{
		cemuLog_logDebug(LogType::Force, "Interpreter: twi at {:08x} ignored", cpu->instructionPointer);
		PPCInterpreter_nextInstruction(cpu);
	}
}
