// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Espresso/PPCState.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "ExceptionHandler.h"

namespace
{
	bool s_crashLogStarted = false;
	bool s_toStdErr = true;
	bool s_toLogFile = true;

	const char* threadStateName(OSThread_t* thread)
	{
		if (thread->suspendCounter != 0)
			return "SUSPENDED";
		switch (thread->state.value())
		{
		case OSThread_t::THREAD_STATE::STATE_NONE: return "NONE";
		case OSThread_t::THREAD_STATE::STATE_READY: return "READY";
		case OSThread_t::THREAD_STATE::STATE_RUNNING: return "RUNNING";
		case OSThread_t::THREAD_STATE::STATE_WAITING: return "WAITING";
		case OSThread_t::THREAD_STATE::STATE_MORIBUND: return "MORIBUND";
		default: return "UNDEFINED";
		}
	}

	void logTitleInfo()
	{
		CrashLog_WriteLine("");
		CrashLog_WriteHeader("Game info");
		if (!CafeSystem::IsTitleRunning())
		{
			CrashLog_WriteLine("Not running");
			return;
		}
		CrashLog_WriteLine("Game: ", false);
		CrashLog_WriteLine(CafeSystem::GetForegroundTitleName());
		CrashLog_WriteLine(fmt::format("TitleId: {:016x}", CafeSystem::GetForegroundTitleId()));
		CrashLog_WriteLine(fmt::format("RPXHash: {:08x} (Update: {:08x})", CafeSystem::GetRPXHashBase(), CafeSystem::GetRPXHashUpdated()));
	}

	void logPPCState()
	{
		CrashLog_WriteLine("");
		CrashLog_WriteHeader("Active PPC instance");
		PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
		if (!cpu)
		{
			CrashLog_WriteLine("Not active");
			return;
		}
		OSThread_t* thread = coreinit::OSGetCurrentThread();
		CrashLog_WriteLine(fmt::format("IP 0x{:08x} LR 0x{:08x} Thread 0x{:08x}", cpu->instructionPointer, cpu->spr.LR, memory_getVirtualOffsetFromPointer(thread)));

		// general purpose registers, eight per line
		CrashLog_WriteLine("");
		for (int row = 0; row < 4; row++)
		{
			std::string line;
			for (int col = 0; col < 8; col++)
			{
				const int r = row * 8 + col;
				if (col)
					line += ' ';
				line += fmt::format("{:<3}={:08x}", fmt::format("r{}", r), cpu->gpr[r]);
			}
			CrashLog_WriteLine(line);
		}

		const MPTR sp = cpu->gpr[1];
		CrashLog_WriteLine("");
		CrashLog_WriteHeader("PPC stack trace");
		DebugLogStackTrace(thread, sp);

		// 16 lines of 8 words from the stack pointer upwards
		CrashLog_WriteLine("");
		CrashLog_WriteHeader("PPC stack dump");
		for (uint32 row = 0; row < 16; row++)
		{
			const MPTR addr = sp + row * 32;
			if (!memory_isAddressRangeAccessible(addr, 32))
			{
				CrashLog_WriteLine(fmt::format("[0x{:08x}] ?", addr));
				continue;
			}
			std::string line = fmt::format("[0x{:08x}]", addr);
			for (uint32 w = 0; w < 8; w++)
			{
				if (w == 4)
					line += " -";
				line += fmt::format(" {:08x}", memory_readU32(addr + w * 4));
			}
			CrashLog_WriteLine(line);
		}
	}

	void logPPCThreads()
	{
		CrashLog_WriteLine("");
		CrashLog_WriteHeader("PPC threads");
		if (activeThreadCount == 0)
			CrashLog_WriteLine("None active");
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			const MPTR threadAddr = activeThread[i];
			OSThread_t* thread = (OSThread_t*)memory_getPointerFromVirtualOffset(threadAddr);
			const uint8 affinity = thread->attr;
			const char* name = thread->threadName.IsNull() ? "NULL" : thread->threadName.GetPtr();
			CrashLog_WriteLine(fmt::format("{:08x} Ent {:08x} IP {:08x} LR {:08x} {:<9} Aff {}{}{} Pri {:2} Name {}",
				threadAddr, thread->entrypoint.GetMPTR(), thread->context.srr0, _swapEndianU32(thread->context.lr),
				threadStateName(thread), affinity & 1, (affinity >> 1) & 1, (affinity >> 2) & 1,
				thread->effectivePriority.value(), name));
		}
	}
}

bool CrashLog_Create()
{
	if (s_crashLogStarted)
		return false;
	s_crashLogStarted = true;
	cemuLog_createLogFile(true);
	CrashLog_SetOutputChannels(true, true);
	return true;
}

void CrashLog_SetOutputChannels(bool writeToStdErr, bool writeToLogTxt)
{
	s_toStdErr = writeToStdErr;
	s_toLogFile = writeToLogTxt;
}

void CrashLog_WriteLine(std::string_view text, bool newLine)
{
	if (s_toLogFile)
		cemuLog_writeLineToLog(text, false, newLine);
	if (s_toStdErr)
	{
		fwrite(text.data(), 1, text.size(), stderr);
		if (newLine)
			fputc('\n', stderr);
	}
}

void CrashLog_WriteHeader(const char* header)
{
	constexpr std::string_view rule = "-----------------------------------------";
	CrashLog_WriteLine(rule);
	CrashLog_WriteLine("   ", false);
	CrashLog_WriteLine(header);
	CrashLog_WriteLine(rule);
}

void ExceptionHandler_LogGeneralInfo()
{
	logTitleInfo();
	logPPCState();
	logPPCThreads();
}
