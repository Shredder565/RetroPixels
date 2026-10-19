// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Coroutine.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/RPL/rpl.h"

namespace coreinit
{
	namespace
	{
		constexpr uint32 kFirstSavedReg = 14; // r14-r31 and f14-f31 are callee saved

		// guest address of the code that finishes OSSwitchCoroutine once the next coroutine returns
		MPTR s_switchCoroutineEpilogue = MPTR_NULL;

		void CaptureCoroutine(OSCoroutine* co, PPCInterpreter_t* cpu)
		{
			co->lr = cpu->spr.LR;
			co->cr = ppc_getCR(cpu);
			co->gqr1 = cpu->spr.UGQR[1];
			co->r1 = cpu->gpr[1];
			co->r2 = cpu->gpr[2];
			co->r13 = cpu->gpr[13];
			for (uint32 i = 0; i < 18; i++)
			{
				co->gpr[i] = cpu->gpr[kFirstSavedReg + i];
				co->fpr[i] = cpu->fpr[kFirstSavedReg + i].fp0int;
				co->psr[i] = cpu->fpr[kFirstSavedReg + i].fp1int;
			}
		}

		void ResumeCoroutine(OSCoroutine* co, PPCInterpreter_t* cpu)
		{
			cpu->spr.LR = co->lr;
			ppc_setCR(cpu, co->cr);
			cpu->spr.UGQR[1] = co->gqr1;
			cpu->gpr[1] = co->r1;
			cpu->gpr[2] = co->r2;
			cpu->gpr[13] = co->r13;
			for (uint32 i = 0; i < 18; i++)
			{
				cpu->gpr[kFirstSavedReg + i] = co->gpr[i];
				cpu->fpr[kFirstSavedReg + i].fp0int = co->fpr[i];
				cpu->fpr[kFirstSavedReg + i].fp1int = co->psr[i];
			}
		}
	}

	// OSInitCoroutine(coroutine, entry, stack): the first switch to it starts at entry on that stack
	void export_OSInitCoroutine(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamStructPtr(co, OSCoroutine, 0);
		ppcDefineParamU32(entry, 1);
		ppcDefineParamU32(stackPointer, 2);
		co->lr = entry;
		co->r1 = stackPointer;
		osLib_returnFromFunction(hCPU, 0);
	}

	// OSSwitchCoroutine(current, next). On the console this sets up a small stack frame, keeps r31
	// in it, saves the current coroutine with r31 holding `next`, and loads `next` with r3 = next
	// and r4 = 1. Titles depend on those details: Injustice: Gods Among Us reads the saved r31 and
	// expects the next coroutine's address there. When the saved coroutine is resumed later it
	// continues in the epilogue below, which pops the frame.
	void export_OSSwitchCoroutine(PPCInterpreter_t* hCPU)
	{
		MPTR currentAddr = hCPU->gpr[3];
		MPTR nextAddr = hCPU->gpr[4];
		uint32 frame = hCPU->gpr[1] - 0x10;
		hCPU->gpr[1] = frame;
		memory_writeU32(frame + 0x0C, hCPU->gpr[31]);
		memory_writeU32(frame + 0x14, hCPU->spr.LR); // LR save word of the caller's frame
		hCPU->spr.LR = s_switchCoroutineEpilogue;
		hCPU->gpr[31] = nextAddr;
		CaptureCoroutine(MEMPTR<OSCoroutine>(currentAddr).GetPtr(), hCPU);
		hCPU->gpr[3] = nextAddr;
		hCPU->gpr[4] = 1;
		ResumeCoroutine(MEMPTR<OSCoroutine>(nextAddr).GetPtr(), hCPU);
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	void SwitchCoroutineEpilogue(PPCInterpreter_t* hCPU)
	{
		uint32 frame = hCPU->gpr[1];
		hCPU->gpr[31] = memory_readU32(frame + 0x0C);
		hCPU->spr.LR = memory_readU32(frame + 0x14);
		hCPU->gpr[1] = frame + 0x10;
		hCPU->instructionPointer = hCPU->spr.LR;
	}

	// OSSwitchFiberEx(a0, a1, a2, a3, func, stack): calls func(a0..a3) on another stack and returns
	// its result
	void export_OSSwitchFiberEx(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(arg0, 0);
		ppcDefineParamU32(arg1, 1);
		ppcDefineParamU32(arg2, 2);
		ppcDefineParamU32(arg3, 3);
		ppcDefineParamMPTR(func, 4);
		ppcDefineParamMPTR(fiberStack, 5);
		MPTR callerStack = hCPU->gpr[1];
		hCPU->gpr[1] = fiberStack;
		hCPU->gpr[3] = arg0;
		hCPU->gpr[4] = arg1;
		hCPU->gpr[5] = arg2;
		hCPU->gpr[6] = arg3;
		PPCCore_executeCallbackInternal(func);
		uint32 result = hCPU->gpr[3];
		hCPU->gpr[1] = callerStack;
		osLib_returnFromFunction(hCPU, result);
	}

	void InitializeCoroutine()
	{
		osLib_addFunction("coreinit", "OSInitCoroutine", export_OSInitCoroutine);
		osLib_addFunction("coreinit", "OSSwitchCoroutine", export_OSSwitchCoroutine);
		osLib_addFunction("coreinit", "OSSwitchFiberEx", export_OSSwitchFiberEx);

		s_switchCoroutineEpilogue = RPLLoader_MakePPCCallable(SwitchCoroutineEpilogue);
	}
}
