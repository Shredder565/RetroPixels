// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Breakpoints, code patches and the stop/step loop of the guest debugger.

#include "Common/precompiled.h"
#include "Debugger.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cemu/PPCAssembler/ppcAssembler.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cemu/ExpressionParser/ExpressionParser.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "OS/RPL/rpl.h"
#include "util/helpers/helpers.h"

#if BOOST_OS_WINDOWS
#include <Windows.h>
#endif

namespace coreinit
{
	std::vector<std::thread::native_handle_type>& OSGetSchedulerThreads();
}

DebuggerDispatcher g_debuggerDispatcher;

namespace
{
	struct
	{
		bool breakOnEntry = false;
		bool logOnlyMemoryBreakpoints = false;

		std::recursive_mutex listMutex;
		std::vector<DebuggerBreakpoint*> breakpoints; // first breakpoint of each address
		std::vector<DebuggerPatch*> patches;
		DebuggerBreakpoint* armedMemoryBreakpoint = nullptr; // at most one at a time

		// the thread currently stopped in the debugger
		std::mutex sessionMutex;
		std::atomic_bool breakRequested{ false };
		std::atomic_bool stopped{ false };
		uint32 stoppedThread = 0;
		PPCInterpreter_t* stoppedCPU = nullptr;
		std::atomic<DebuggerStepCommand> pendingStep{ DebuggerStepCommand::None };
	} s_dbg;

	// ---- breakpoint chains ----

	DebuggerBreakpoint* FindChainWithType(uint32 address, uint8 bpType)
	{
		for (DebuggerBreakpoint* bp = debugger_getFirstBP(address); bp; bp = bp->next)
		{
			if (bp->bpType == bpType)
				return bp;
		}
		return nullptr;
	}

	bool ChainContainsType(DebuggerBreakpoint* chain, uint8 bpType)
	{
		for (; chain; chain = chain->next)
		{
			if (chain->bpType == bpType)
				return true;
		}
		return false;
	}

	void AppendToChain(DebuggerBreakpoint* bp)
	{
		bp->next = nullptr;
		DebuggerBreakpoint* tail = debugger_getFirstBP(bp->address);
		if (!tail)
		{
			s_dbg.breakpoints.push_back(bp);
			return;
		}
		while (tail->next)
			tail = tail->next;
		tail->next = bp;
	}

	// the instruction at address as the game sees it (ignoring our traps)
	uint32 OriginalInstruction(uint32 address)
	{
		for (DebuggerBreakpoint* bp = debugger_getFirstBP(address); bp; bp = bp->next)
		{
			if (bp->isExecuteBP())
				return bp->originalOpcodeValue;
		}
		return memory_readU32(address);
	}

	void WriteCodeWord(uint32 address, uint32 value)
	{
		bool changed = memory_readU32(address) != value;
		memory_writeU32(address, value);
		if (changed)
			PPCRecompiler_invalidateRange(address, address + 4);
	}

	// Puts a trap at address while any execute breakpoint there is enabled,
	// otherwise puts the original instruction back. restoreOnly forces the
	// original instruction (used to step over a breakpoint).
	void SyncTrapInstruction(uint32 address, bool restoreOnly = false)
	{
		bool anyExecute = false;
		bool anyEnabled = false;
		uint32 original = 0;
		for (DebuggerBreakpoint* bp = debugger_getFirstBP(address); bp; bp = bp->next)
		{
			if (!bp->isExecuteBP())
				continue;
			anyExecute = true;
			anyEnabled |= bp->enabled;
			original = bp->originalOpcodeValue;
		}
		if (!anyExecute)
			return;
		if (anyEnabled && !restoreOnly)
			WriteCodeWord(address, DEBUGGER_BP_T_DEBUGGER_TW);
		else
			WriteCodeWord(address, original);
	}

	// ---- memory breakpoints (host debug registers) ----

	void ArmMemoryBreakpoint(DebuggerBreakpoint* bp)
	{
		std::vector<std::thread::native_handle_type> threads = coreinit::OSGetSchedulerThreads();
#if BOOST_OS_WINDOWS
		s_dbg.armedMemoryBreakpoint = bp;
		for (auto& nativeHandle : threads)
		{
			HANDLE thread = (HANDLE)nativeHandle;
			CONTEXT ctx{};
			ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			SuspendThread(thread);
			GetThreadContext(thread, &ctx);
			// DR0 watches writes, DR1 reads and writes, both on the same 4 bytes
			const uint32 enable = bp ? 1 : 0;
			const uint32 length4 = bp ? 3 : 0;
			if (bp)
			{
				ctx.Dr0 = (DWORD64)memory_getPointerFromVirtualOffset(bp->address);
				ctx.Dr1 = (DWORD64)memory_getPointerFromVirtualOffset(bp->address);
			}
			SetBits(ctx.Dr7, 0, 1, enable);
			SetBits(ctx.Dr7, 16, 2, bp ? 1 : 0); // DR0: break on write
			SetBits(ctx.Dr7, 18, 2, length4);
			SetBits(ctx.Dr7, 2, 1, enable);
			SetBits(ctx.Dr7, 20, 2, bp ? 3 : 0); // DR1: break on read or write
			SetBits(ctx.Dr7, 22, 2, length4);
			SetThreadContext(thread, &ctx);
			ResumeThread(thread);
		}
#else
		cemuLog_log(LogType::Force, "Debugger breakpoints are not supported");
#endif
	}

	// ---- stopping ----

	bool IsCallInstruction(MPTR address)
	{
		PPCDisassembledInstruction instr = { 0 };
		ppcAssembler_disassemble(address, OriginalInstruction(address), &instr);
		return instr.ppcAsmCode == PPCASM_OP_BL || instr.ppcAsmCode == PPCASM_OP_BLA || instr.ppcAsmCode == PPCASM_OP_BCTRL;
	}

	// expands {rN} and {fN} in a logging breakpoint comment
	void ExpandRegisterPlaceholders(std::string& text, std::string_view prefix, const std::function<std::string(int)>& format)
	{
		size_t pos = 0;
		while ((pos = text.find(prefix, pos)) != std::string::npos)
		{
			size_t close = text.find('}', pos);
			if (close == std::string::npos)
				return;
			std::string_view number = std::string_view(text).substr(pos + prefix.size(), close - pos - prefix.size());
			int reg = -1;
			try
			{
				reg = ConvertString<int>(number);
			}
			catch (...)
			{
				reg = -1;
			}
			if (reg < 0 || reg >= 32)
			{
				pos = close + 1;
				continue;
			}
			std::string value = format(reg);
			text.replace(pos, close - pos + 1, value);
			pos += value.size();
		}
	}

	void LogBreakpointHit(PPCInterpreter_t* cpu, DebuggerBreakpoint* bp)
	{
		std::string message = bp->comment.empty() ? fmt::format("Breakpoint at 0x{:08X} (no comment)", bp->address) : boost::nowide::narrow(bp->comment);
		ExpandRegisterPlaceholders(message, "{r", [cpu](int reg) { return fmt::format("0x{:08X}", cpu->gpr[reg]); });
		ExpandRegisterPlaceholders(message, "{f", [cpu](int reg) { return fmt::format("{}", cpu->fpr[reg].fpr); });
		uint32 thread = MEMPTR<OSThread_t>(coreinit::OSGetCurrentThread()).GetMPTR();
		cemuLog_log(LogType::Force, "[Debugger] Breakpoint '{}' was executed! Thread: {:08x} LR: 0x{:08x}", message, thread, cpu->spr.LR);
		if (cemuLog_advancedPPCLoggingEnabled())
			DebugLogStackTrace(coreinit::OSGetCurrentThread(), cpu->gpr[1]);
	}

	// runs the instruction under the trap at the current IP
	void ExecuteOverTrap(PPCInterpreter_t* cpu)
	{
		std::unique_lock lock(s_dbg.listMutex);
		PPCRecompiler_Enable();
		MPTR address = cpu->instructionPointer;
		SyncTrapInstruction(address, true);
		PPCInterpreterSlim_executeInstruction(cpu);
		SyncTrapInstruction(address);
		PPCRecompiler_Disable();
	}

	// blocks the current thread until the UI resumes it, returns the step request
	DebuggerStepCommand WaitForUserCommand(PPCInterpreter_t* cpu)
	{
		DebuggerStepCommand command = DebuggerStepCommand::Run;
		if (DebuggerBreakpoint* oneShot = FindChainWithType(cpu->instructionPointer, DEBUGGER_BP_T_ONE_SHOT))
			debugger_deleteBreakpoint(oneShot->id);
		g_debuggerDispatcher.NotifyDebugBreakpointHit();
		g_debuggerDispatcher.UpdateViewThreadsafe();
		while (s_dbg.stopped)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			command = s_dbg.pendingStep.exchange(DebuggerStepCommand::None);
			if (command != DebuggerStepCommand::None)
				break;
			if (!coreinit::OSIsSchedulerActive())
			{
				command = DebuggerStepCommand::Run; // emulation is shutting down
				break;
			}
		}
		// stepping over anything but a call is the same as stepping into it
		if (command == DebuggerStepCommand::StepOver && !IsCallInstruction(cpu->instructionPointer))
			command = DebuggerStepCommand::StepInto;
		g_debuggerDispatcher.UpdateViewThreadsafe();
		g_debuggerDispatcher.NotifyRun();
		return command;
	}
}

// ---- breakpoint list ----

std::vector<DebuggerBreakpoint*>& debugger_lockBreakpoints()
{
	s_dbg.listMutex.lock();
	return s_dbg.breakpoints;
}

void debugger_unlockBreakpoints()
{
	s_dbg.listMutex.unlock();
}

DebuggerBreakpoint* debugger_getFirstBP(uint32 address)
{
	auto it = std::find_if(s_dbg.breakpoints.begin(), s_dbg.breakpoints.end(), [address](DebuggerBreakpoint* bp) { return bp->address == address; });
	return it != s_dbg.breakpoints.end() ? *it : nullptr;
}

DebuggerBreakpoint* debugger_getBreakpointById(BreakpointId bpId)
{
	for (DebuggerBreakpoint* chain : s_dbg.breakpoints)
	{
		for (DebuggerBreakpoint* bp = chain; bp; bp = bp->next)
		{
			if (bp->id == bpId)
				return bp;
		}
	}
	return nullptr;
}

void debugger_createCodeBreakpoint(uint32 address, uint8 bpType)
{
	if (ChainContainsType(debugger_getFirstBP(address), bpType))
		return;
	AppendToChain(new DebuggerBreakpoint(address, OriginalInstruction(address), bpType, true));
	SyncTrapInstruction(address);
}

void debugger_createMemoryBreakpoint(uint32 address, bool onRead, bool onWrite)
{
	// read and write at once is not supported, such a request becomes a write breakpoint
	cemu_assert_debug(!(onRead && onWrite));
	uint8 bpType = (onRead && !onWrite) ? DEBUGGER_BP_T_MEMORY_READ : DEBUGGER_BP_T_MEMORY_WRITE;
	DebuggerBreakpoint* bp = new DebuggerBreakpoint(address, 0xFFFFFFFF, bpType, true);
	AppendToChain(bp);
	if (s_dbg.armedMemoryBreakpoint)
	{
		s_dbg.armedMemoryBreakpoint->enabled = false;
		s_dbg.armedMemoryBreakpoint = nullptr;
	}
	ArmMemoryBreakpoint(bp);
}

void debugger_deleteBreakpoint(BreakpointId bpId)
{
	std::unique_lock lock(s_dbg.listMutex);
	DebuggerBreakpoint* bp = debugger_getBreakpointById(bpId);
	if (!bp)
		return;
	const uint32 address = bp->address;
	DebuggerBreakpoint* head = debugger_getFirstBP(address);
	if (head == bp)
	{
		// the rest of the chain (if any) moves to the end of the list
		s_dbg.breakpoints.erase(std::remove(s_dbg.breakpoints.begin(), s_dbg.breakpoints.end(), bp), s_dbg.breakpoints.end());
		if (bp->next)
			s_dbg.breakpoints.push_back(bp->next);
	}
	else
	{
		DebuggerBreakpoint* prev = head;
		while (prev->next != bp)
			prev = prev->next;
		prev->next = bp->next;
	}
	if (bp->isExecuteBP())
	{
		bool otherExecute = false;
		for (DebuggerBreakpoint* other = debugger_getFirstBP(address); other; other = other->next)
			otherExecute |= other->isExecuteBP();
		if (otherExecute)
			SyncTrapInstruction(address);
		else
			WriteCodeWord(address, bp->originalOpcodeValue); // last execute breakpoint here: restore the code
	}
	if (s_dbg.armedMemoryBreakpoint == bp)
		ArmMemoryBreakpoint(nullptr);
	delete bp;
}

void debugger_toggleExecuteBreakpoint(uint32 address)
{
	std::unique_lock lock(s_dbg.listMutex);
	if (DebuggerBreakpoint* bp = FindChainWithType(address, DEBUGGER_BP_T_NORMAL))
		debugger_deleteBreakpoint(bp->id);
	else
		debugger_createCodeBreakpoint(address, DEBUGGER_BP_T_NORMAL);
}

void debugger_toggleLoggingBreakpoint(uint32 address)
{
	std::unique_lock lock(s_dbg.listMutex);
	if (DebuggerBreakpoint* bp = FindChainWithType(address, DEBUGGER_BP_T_LOGGING))
		debugger_deleteBreakpoint(bp->id);
	else
		debugger_createCodeBreakpoint(address, DEBUGGER_BP_T_LOGGING);
}

void debugger_toggleBreakpoint(uint32 address, bool state, DebuggerBreakpoint* bp)
{
	DebuggerBreakpoint* itr = debugger_getFirstBP(address);
	while (itr && itr != bp)
		itr = itr->next;
	if (!itr || bp->enabled == state)
		return;
	if (bp->bpType == DEBUGGER_BP_T_NORMAL || bp->bpType == DEBUGGER_BP_T_LOGGING)
	{
		bp->enabled = state;
		SyncTrapInstruction(address);
		g_debuggerDispatcher.UpdateViewThreadsafe();
	}
	else if (bp->isMemBP())
	{
		// only one memory breakpoint can be active
		for (DebuggerBreakpoint* chain : s_dbg.breakpoints)
		{
			for (DebuggerBreakpoint* other = chain; other; other = other->next)
			{
				if (other != bp && other->isMemBP())
					other->enabled = false;
			}
		}
		bp->enabled = state;
		ArmMemoryBreakpoint(state ? bp : nullptr);
		g_debuggerDispatcher.UpdateViewThreadsafe();
	}
}

void debugger_handleEntryBreakpoint(uint32 address)
{
	if (s_dbg.breakOnEntry)
		debugger_createCodeBreakpoint(address, DEBUGGER_BP_T_NORMAL);
}

// called by the exception handler when a debug register fired (Windows)
void debugger_handleSingleStepException(uint64 dr6)
{
	DebuggerBreakpoint* bp = s_dbg.armedMemoryBreakpoint;
	if (!bp)
		return;
	// DR0 only watches writes, so DR0+DR1 means a write and DR1 alone a read
	const bool hitDR0 = (dr6 & 1) != 0;
	const bool hitDR1 = (dr6 & 2) != 0;
	const bool isWrite = hitDR0 && hitDR1;
	if (bp->bpType != (isWrite ? DEBUGGER_BP_T_MEMORY_WRITE : DEBUGGER_BP_T_MEMORY_READ))
		return;
	PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
	if (!s_dbg.logOnlyMemoryBreakpoints)
	{
		// stop right after the accessing instruction
		debugger_createCodeBreakpoint(cpu->instructionPointer + 4, DEBUGGER_BP_T_ONE_SHOT);
		return;
	}
	cemuLog_log(LogType::Force, "[Debugger] 0x{:08X} was read/written! New Value: 0x{:08X} (float {}) IP: {:08X} LR: {:08X}",
		bp->address, memory_readU32(bp->address), memory_readFloat(bp->address), cpu->instructionPointer, cpu->spr.LR);
	if (cemuLog_advancedPPCLoggingEnabled())
		DebugLogStackTrace(coreinit::OSGetCurrentThread(), cpu->gpr[1]);
}

// ---- patches ----

void debugger_createPatch(uint32 address, std::span<uint8> patchData)
{
	cemu_assert_debug((address & 3) == 0);
	cemu_assert_debug((patchData.size() & 3) == 0);
	const uint32 wordCount = (uint32)(patchData.size() / 4);

	DebuggerPatch* patch = new DebuggerPatch();
	patch->address = address;
	patch->length = (sint32)patchData.size();
	patch->data.assign(patchData.begin(), patchData.end());
	patch->origData.resize(patchData.size());
	memcpy(patch->origData.data(), memory_getPointerFromVirtualOffset(address), patchData.size());
	// words covered by a trap: the original instruction is kept in the breakpoint
	for (uint32 i = 0; i < wordCount; i++)
	{
		for (DebuggerBreakpoint* bp = debugger_getFirstBP(address + i * 4); bp; bp = bp->next)
		{
			if (bp->isExecuteBP())
			{
				uint32be original = bp->originalOpcodeValue;
				memcpy(patch->origData.data() + i * 4, &original, 4);
			}
		}
	}

	// merge with patches that overlap or touch; the oldest original bytes win
	for (size_t i = 0; i < s_dbg.patches.size();)
	{
		DebuggerPatch* other = s_dbg.patches[i];
		const uint32 begin = patch->address, end = patch->address + patch->length;
		const uint32 otherBegin = other->address, otherEnd = other->address + other->length;
		if (end < otherBegin || begin > otherEnd)
		{
			i++;
			continue;
		}
		const uint32 mergedBegin = std::min(begin, otherBegin);
		const uint32 mergedEnd = std::max(end, otherEnd);
		DebuggerPatch* merged = new DebuggerPatch();
		merged->address = mergedBegin;
		merged->length = (sint32)(mergedEnd - mergedBegin);
		merged->data.resize(merged->length);
		merged->origData.resize(merged->length);
		memcpy(merged->data.data() + (otherBegin - mergedBegin), other->data.data(), other->length);
		memcpy(merged->data.data() + (begin - mergedBegin), patch->data.data(), patch->length);
		memcpy(merged->origData.data() + (begin - mergedBegin), patch->origData.data(), patch->length);
		memcpy(merged->origData.data() + (otherBegin - mergedBegin), other->origData.data(), other->length);
		delete patch;
		delete other;
		patch = merged;
		s_dbg.patches.erase(s_dbg.patches.begin() + i);
	}
	s_dbg.patches.push_back(patch);

	// write the new code; under an enabled trap only the saved instruction changes
	for (uint32 i = 0; i < wordCount; i++)
	{
		const uint32 wordAddress = address + i * 4;
		bool trapped = false;
		for (DebuggerBreakpoint* bp = debugger_getFirstBP(wordAddress); bp; bp = bp->next)
		{
			if (!bp->isExecuteBP())
				continue;
			bp->originalOpcodeValue = *(uint32be*)(patchData.data() + i * 4);
			trapped |= bp->enabled;
		}
		if (!trapped)
		{
			memcpy(memory_getPointerFromVirtualOffset(wordAddress), patchData.data() + i * 4, 4);
			PPCRecompiler_invalidateRange(wordAddress, wordAddress + 4);
		}
	}
}

bool debugger_hasPatch(uint32 address)
{
	return std::any_of(s_dbg.patches.begin(), s_dbg.patches.end(), [address](DebuggerPatch* patch) {
		return address + 4 > patch->address && address < patch->address + patch->length;
	});
}

void debugger_removePatch(uint32 address)
{
	auto it = std::find_if(s_dbg.patches.begin(), s_dbg.patches.end(), [address](DebuggerPatch* patch) {
		return address >= patch->address && address < patch->address + patch->length;
	});
	if (it == s_dbg.patches.end())
		return;
	DebuggerPatch* patch = *it;
	const MPTR begin = patch->address;
	const MPTR end = patch->address + patch->length;
	// breakpoints inside the patched range are switched off
	for (DebuggerBreakpoint* bp : s_dbg.breakpoints)
	{
		if (bp->address + 4 > begin && bp->address < end)
		{
			bp->enabled = false;
			SyncTrapInstruction(bp->address);
		}
	}
	memcpy(MEMPTR<void>(begin).GetPtr(), patch->origData.data(), patch->length);
	PPCRecompiler_invalidateRange(begin, end);
	delete patch;
	s_dbg.patches.erase(it);
}

// ---- debug session ----

bool debugger_isTrapped()
{
	return s_dbg.stopped;
}

PPCInterpreter_t* debugger_lockDebugSession()
{
	s_dbg.sessionMutex.lock();
	if (s_dbg.stopped)
		return s_dbg.stoppedCPU; // stays locked until debugger_unlockDebugSession
	s_dbg.sessionMutex.unlock();
	return nullptr;
}

void debugger_unlockDebugSession(PPCInterpreter_t* hCPU)
{
	cemu_assert_debug(s_dbg.stopped && s_dbg.stoppedCPU == hCPU);
	s_dbg.sessionMutex.unlock();
}

PPCSnapshot debugger_getSnapshotFromSession(PPCInterpreter_t* hCPU)
{
	cemu_assert_debug(s_dbg.stopped && s_dbg.stoppedCPU == hCPU);
	PPCSnapshot snapshot{};
	std::copy(std::begin(hCPU->gpr), std::end(hCPU->gpr), snapshot.gpr);
	std::copy(std::begin(hCPU->fpr), std::end(hCPU->fpr), snapshot.fpr);
	std::copy(std::begin(hCPU->cr), std::end(hCPU->cr), snapshot.cr);
	snapshot.spr_lr = hCPU->spr.LR;
	return snapshot;
}

void debugger_stepCommand(DebuggerStepCommand stepCommand)
{
	s_dbg.pendingStep = stepCommand;
}

void debugger_requestBreak()
{
	s_dbg.breakRequested = true;
}

// entered from the interpreter when it executes one of our traps
void debugger_enterTW(PPCInterpreter_t* hCPU, bool isSingleStep)
{
	// only one thread can be stopped at a time
	for (;;)
	{
		std::unique_lock lock(s_dbg.sessionMutex);
		bool expected = false;
		if (s_dbg.stopped.compare_exchange_strong(expected, true))
		{
			s_dbg.stoppedThread = MEMPTR<OSThread_t>(coreinit::OSGetCurrentThread()).GetMPTR();
			s_dbg.stoppedCPU = hCPU;
			s_dbg.pendingStep = DebuggerStepCommand::None;
			break;
		}
		lock.unlock();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	DebuggerBreakpoint* chain = debugger_getFirstBP(hCPU->instructionPointer);
	const bool stops = ChainContainsType(chain, DEBUGGER_BP_T_NORMAL) || ChainContainsType(chain, DEBUGGER_BP_T_ONE_SHOT);
	for (DebuggerBreakpoint* bp = chain; bp; bp = bp->next)
	{
		if (bp->bpType == DEBUGGER_BP_T_LOGGING && bp->enabled)
			LogBreakpointHit(hCPU, bp);
	}

	DebuggerStepCommand command = DebuggerStepCommand::Run;
	if (stops || isSingleStep)
		command = WaitForUserCommand(hCPU);

	{
		std::unique_lock lock(s_dbg.sessionMutex);
		s_dbg.stoppedCPU = nullptr;
		s_dbg.stopped = false;
	}

	const MPTR trapAddress = hCPU->instructionPointer;
	ExecuteOverTrap(hCPU);
	if (command == DebuggerStepCommand::StepInto)
		debugger_enterTW(hCPU, true);
	else if (command == DebuggerStepCommand::StepOver)
		debugger_createCodeBreakpoint(trapAddress + 4, DEBUGGER_BP_T_ONE_SHOT);
}

// module base addresses and (when stopped) GPRs for address expressions
void debugger_addParserSymbols(class ExpressionParser& ep)
{
	for (RPLModule* module : RPLLoader_GetModuleList())
	{
		if (module)
			ep.AddConstant(module->moduleName, (double)module->regionMappingBase_text.GetMPTR());
	}
	if (PPCInterpreter_t* cpu = debugger_lockDebugSession())
	{
		PPCSnapshot snapshot = debugger_getSnapshotFromSession(cpu);
		for (sint32 i = 0; i < 32; i++)
			ep.AddConstant(fmt::format("r{}", i), snapshot.gpr[i]);
		debugger_unlockDebugSession(cpu);
	}
}

void debugger_jumpToAddressInDisasm(MPTR address)
{
	g_debuggerDispatcher.MoveToAddressInDisassembly(address);
}

// ---- options ----

void debugger_setOptionBreakOnEntry(bool isEnabled)
{
	s_dbg.breakOnEntry = isEnabled;
}

bool debugger_getOptionBreakOnEntry()
{
	return s_dbg.breakOnEntry;
}

void debugger_setOptionLogOnlyMemoryBreakpoints(bool isEnabled)
{
	s_dbg.logOnlyMemoryBreakpoints = isEnabled;
}

bool debugger_getOptionLogOnlyMemoryBreakpoints()
{
	return s_dbg.logOnlyMemoryBreakpoints;
}
