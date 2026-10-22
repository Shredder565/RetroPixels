// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Recompiler runtime: jump table, compile requests and the background
// compiler thread, entering and leaving generated code, invalidation.

#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "PPCFunctionBoundaryTracker.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"
#include "IML/IML.h"
#include "IML/IMLRegisterAllocator.h"
#include "util/containers/RangeStore.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Common/FileStream.h"
#include "util/helpers/fspinlock.h"
#include "util/helpers/helpers.h"
#include "util/MemMapper/MemMapper.h"

#if defined(ARCH_X86_64)
#include "BackendX64/BackendX64.h"
#elif defined(__aarch64__)
#include "BackendAArch64/BackendAArch64.h"
#endif

void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_visited)();
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_unvisited)();

PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;

namespace
{
	// jump table memory is committed in steps covering 4MB of guest code
	constexpr uint32 kTableStep = 4 * 1024 * 1024;
	constexpr uint32 kTableStepCount = (MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE + kTableStep - 1) / kTableStep;

	struct
	{
		std::atomic_bool initialized{false};
		std::atomic<sint32> enableCount{0};
		FSpinlock lock; // protects the jump table writes, the queue, the invalidations and the function store
		std::queue<uint32> requests;
		// invalidations that happened while a function was being compiled
		std::vector<std::pair<uint32, uint32>> invalidatedDuringCompile;
		RangeStore<PPCRecCompiledFunction*, uint32, 7703, 0x2000> functions;
		std::bitset<kTableStepCount> tableStepCommitted;
		std::thread worker;
		std::atomic_bool stopWorker{false};
	}s_rec;

	PPCREC_JUMP_ENTRY* JumpTable()
	{
		return ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
	}

	bool HasTableEntry(uint32 address)
	{
		if (address >= MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE)
			return false;
		return s_rec.tableStepCommitted[address / kTableStep];
	}

	bool IsCompiledEntry(PPCREC_JUMP_ENTRY e)
	{
		return e != PPCRecompiler_leaveRecompilerCode_unvisited && e != PPCRecompiler_leaveRecompilerCode_visited;
	}

	// queues the address for compilation unless that happened before. Never
	// blocks: if the lock is busy the request is dropped (the address is
	// requested again the next time execution gets there)
	void RequestCompilation(uint32 address)
	{
		PPCREC_JUMP_ENTRY* table = JumpTable();
		if (table[address / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
			return;
		if (!s_rec.lock.try_lock())
			return;
		if (table[address / 4] == PPCRecompiler_leaveRecompilerCode_unvisited)
		{
			s_rec.requests.emplace(address);
			table[address / 4] = PPCRecompiler_leaveRecompilerCode_visited;
		}
		s_rec.lock.unlock();
	}

	void RunCompiledCode(PPCInterpreter_t* hCPU, PPCREC_JUMP_ENTRY code)
	{
#if BOOST_OS_WINDOWS
		// generated code expects round-to-nearest
		uint32 previousCsr = _mm_getcsr();
		bool restoreRounding = (previousCsr & _MM_ROUND_MASK) != 0;
		if (restoreRounding) [[unlikely]]
			_mm_setcsr(previousCsr & ~_MM_ROUND_MASK);
		PPCRecompiler_enterRecompilerCode((uint64)code, (uint64)hCPU);
		if (restoreRounding) [[unlikely]]
			_mm_setcsr((_mm_getcsr() & ~_MM_ROUND_MASK) | (previousCsr & _MM_ROUND_MASK));
#else
		PPCRecompiler_enterRecompilerCode((uint64)code, (uint64)hCPU);
#endif
		// generated code left early: ask for the code where execution continues
		if (hCPU->remainingCycles > 0)
			RequestCompilation(hCPU->instructionPointer);
	}

	bool InCodeGenArea(uint32 address)
	{
		uint32 start = 0, size = 0;
		coreinit::OSGetCodegenVirtAddrRangeInternal(start, size);
		return size != 0 && address >= start && address < start + size;
	}

	PPCRecCompiledFunction* CompileFunction(uint32 entryAddress, const PPCFunctionBoundaryTracker& tracker, std::vector<std::pair<uint32, uint32>>& entryPointsOut)
	{
		if (entryAddress >= PPC_REC_CODE_AREA_END)
		{
			cemuLog_log(LogType::Force, "Recompiler: function at 0x{:08x} is outside of the code area", entryAddress);
			return nullptr;
		}
		// code written at runtime (JIT engines in games) is left to the interpreter when asked to
		if (InCodeGenArea(entryAddress) && coreinit::codeGenShouldAvoid())
			return nullptr;
		uint32 lowerLimit = LaunchSettings::GetPPCRecLowerAddr();
		uint32 upperLimit = LaunchSettings::GetPPCRecUpperAddr();
		if (lowerLimit != 0 && upperLimit != 0 && (entryAddress < lowerLimit || entryAddress > upperLimit))
			return nullptr;

		IMLFunction func;
		PPCIMLTranslator translator(func, tracker);
		if (!translator.Translate(entryAddress))
			return nullptr;
		PPCRecompilerIML_IsolateEntries(func);
		func.RemoveUnreachable();
		IMLRegisterAllocator_PrepareGuestRegisters(func);
		IMLOptimizer_Run(func);
#ifdef PPCREC_VERIFY_IML
		// consistency checks of the intermediate code (test builds)
		std::string problem;
		if (!IMLAnalyzer_Verify(func, &problem))
		{
			cemuLog_log(LogType::Force, "Recompiler: invalid intermediate code for 0x{:08x}: {}", entryAddress, problem);
			IMLDebug_Dump(func);
			return nullptr;
		}
#endif

		PPCRecCompiledFunction* result = new PPCRecCompiledFunction();
		result->entryAddress = entryAddress;
		bool generated = false;
#if defined(ARCH_X86_64)
		if (IMLRegisterAllocator_AllocateRegisters(func, X64Backend::GetAllocatorParameters()))
			generated = PPCRecompiler_generateX64Code(result, func, entryPointsOut);
#elif defined(__aarch64__)
		if (IMLRegisterAllocator_AllocateRegisters(func, AArch64Backend_GetAllocatorParameters()))
			generated = PPCRecompiler_generateAArch64Code(result, func, entryPointsOut);
#endif
		if (!generated)
		{
			delete result;
			return nullptr;
		}
		for (const auto& r : tracker.GetRanges())
			result->ranges.push_back({r.start, r.end, nullptr});

		if (ActiveSettings::DumpRecompilerFunctionsEnabled())
		{
			FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(fmt::format("dump/recompiler/ppc_{:08x}.bin", entryAddress)));
			if (fs)
			{
				fs->writeData(result->hostCode, result->hostCodeSize);
				delete fs;
			}
		}
		return result;
	}

	// publishes the entry points of a compiled function in the jump table
	bool ActivateFunction(uint32 requestedAddress, PPCRecCompiledFunction* func, const std::vector<std::pair<uint32, uint32>>& entryPoints)
	{
		PPCREC_JUMP_ENTRY* table = JumpTable();
		s_rec.lock.lock();
		// the request may have been dropped by an invalidation while compiling
		if (table[requestedAddress / 4] != PPCRecompiler_leaveRecompilerCode_visited)
		{
			s_rec.lock.unlock();
			return false;
		}
		for (const auto& [invStart, invEnd] : s_rec.invalidatedDuringCompile)
		{
			for (const auto& r : func->ranges)
			{
				if (r.start < invEnd && r.end > invStart)
				{
					s_rec.lock.unlock();
					return false;
				}
			}
		}
		for (const auto& [guestAddress, offset] : entryPoints)
		{
			if (!HasTableEntry(guestAddress))
				continue;
			void* host = (uint8*)func->hostCode + offset;
			table[guestAddress / 4] = (PPCREC_JUMP_ENTRY)host;
			func->entryPoints.emplace_back(guestAddress, host);
		}
		// addresses inside the function that were requested but did not get an
		// entry point are requested again when execution reaches them
		for (const auto& r : func->ranges)
		{
			for (uint32 a = r.start; a < r.end; a += 4)
			{
				if (HasTableEntry(a) && table[a / 4] == PPCRecompiler_leaveRecompilerCode_visited)
					table[a / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
			}
		}
		for (auto& r : func->ranges)
			r.storeHandle = s_rec.functions.storeRange(func, r.start, r.end);
		s_rec.lock.unlock();
		return true;
	}

	void CompileRequested(uint32 address)
	{
		s_rec.lock.lock();
		s_rec.invalidatedDuringCompile.clear();
		s_rec.lock.unlock();

		PPCFunctionBoundaryTracker tracker;
		tracker.Discover(address);
		std::vector<std::pair<uint32, uint32>> entryPoints;
		PPCRecCompiledFunction* func = CompileFunction(address, tracker, entryPoints);
		if (!func)
			return; // stays marked as visited, the interpreter keeps running it
		if (!ActivateFunction(address, func, entryPoints))
			delete func; // the generated code itself is not reclaimed
	}

	void CompilerThread()
	{
		SetThreadName("PPCRecompiler");
		while (!s_rec.stopWorker)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			while (!s_rec.stopWorker)
			{
				s_rec.lock.lock();
				if (s_rec.requests.empty())
				{
					s_rec.lock.unlock();
					break;
				}
				uint32 address = s_rec.requests.front();
				s_rec.requests.pop();
				bool stillWanted = JumpTable()[address / 4] == PPCRecompiler_leaveRecompilerCode_visited;
				s_rec.lock.unlock();
				if (stillWanted)
					CompileRequested(address);
			}
		}
	}

	void CommitTableStep(uint32 stepIndex)
	{
		if (s_rec.tableStepCommitted[stepIndex])
			return;
		PPCREC_JUMP_ENTRY* first = JumpTable() + (size_t)stepIndex * (kTableStep / 4);
		void* p = MemMapper::AllocateMemory(first, (kTableStep / 4) * sizeof(PPCREC_JUMP_ENTRY), MemMapper::PAGE_PERMISSION::P_RW, true);
		if (!p)
		{
			cemuLog_log(LogType::Force, "Recompiler: failed to commit the jump table for 0x{:08x}", stepIndex * kTableStep);
			cemu_assert(false);
			return;
		}
		for (uint32 i = 0; i < kTableStep / 4; i++)
			first[i] = PPCRecompiler_leaveRecompilerCode_unvisited;
		s_rec.tableStepCommitted[stepIndex] = true;
	}

	void DeleteFunction(PPCRecCompiledFunction* func)
	{
		PPCREC_JUMP_ENTRY* table = JumpTable();
		for (auto& [guestAddress, host] : func->entryPoints)
		{
			if (table[guestAddress / 4] == (PPCREC_JUMP_ENTRY)host)
				table[guestAddress / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
		}
		for (auto& r : func->ranges)
		{
			if (r.storeHandle)
				s_rec.functions.deleteRange(r.storeHandle);
			r.storeHandle = nullptr;
		}
		// the host code is not freed, another core may still be running it
		delete func;
	}
}

void PPCRecompiler_recompileIfUnvisited(uint32 enterAddress)
{
	if (s_rec.enableCount <= 0)
		return;
	RequestCompilation(enterAddress);
}

void PPCRecompiler_attemptEnter(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_rec.enableCount <= 0 || hCPU->remainingCycles <= 0)
		return;
	PPCREC_JUMP_ENTRY code = JumpTable()[enterAddress / 4];
	if (code == PPCRecompiler_leaveRecompilerCode_unvisited)
		RequestCompilation(enterAddress);
	else if (code != PPCRecompiler_leaveRecompilerCode_visited)
		RunCompiledCode(hCPU, code);
}

void PPCRecompiler_attemptEnterWithoutRecompile(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (s_rec.enableCount <= 0)
		return;
	PPCREC_JUMP_ENTRY code = JumpTable()[enterAddress / 4];
	if (IsCompiledEntry(code))
		RunCompiledCode(hCPU, code);
}

void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)
{
	if (!ppcRecompilerInstanceData)
		return;
	uint32 limit = MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE;
	uint32 first = std::min(startAddress, limit) / kTableStep;
	uint32 end = (uint32)std::min<uint64>(((uint64)startAddress + size + kTableStep - 1) / kTableStep, kTableStepCount);
	for (uint32 i = first; i < end; i++)
		CommitTableStep(i);
}

void PPCRecompiler_invalidateRange(uint32 startAddr, uint32 endAddr)
{
	if (!s_rec.initialized)
		return;
	if (startAddr >= PPC_REC_CODE_AREA_SIZE)
		return;
	cemu_assert_debug(endAddr >= startAddr);
	s_rec.lock.lock();
	uint32 rStart, rEnd;
	PPCRecCompiledFunction* func;
	while (s_rec.functions.findFirstRange(startAddr, endAddr, rStart, rEnd, func))
		DeleteFunction(func);
	s_rec.invalidatedDuringCompile.emplace_back(startAddr, endAddr);
	s_rec.lock.unlock();
}

extern "C" DLLEXPORT uintptr_t* PPCRecompiler_getJumpTableBase()
{
	if (!ppcRecompilerInstanceData)
		return nullptr;
	return (uintptr_t*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
}

void PPCRecompiler_init()
{
	s_rec.enableCount = 0;
	if (ActiveSettings::GetCPUMode() == CPUMode::SinglecoreInterpreter)
	{
		cemuLog_log(LogType::Force, "Using singlecore interpreter");
		return;
	}
	if (LaunchSettings::ForceInterpreter() || LaunchSettings::ForceMultiCoreInterpreter())
	{
		cemuLog_log(LogType::Force, "Recompiler disabled. Command line --force-interpreter or force-multicore-interpreter was passed");
		return;
	}
#if !defined(ARCH_X86_64) && !defined(__aarch64__)
	cemuLog_log(LogType::Force, "Recompiler not available on this architecture");
	return;
#endif
	if (ppcRecompilerInstanceData)
	{
		MemMapper::FreeReservation(ppcRecompilerInstanceData, sizeof(PPCRecompilerInstanceData_t));
		ppcRecompilerInstanceData = nullptr;
	}
	ppcRecompilerInstanceData = (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, sizeof(PPCRecompilerInstanceData_t), MemMapper::PAGE_PERMISSION::P_RW);
	s_rec.tableStepCommitted.reset();
#if defined(ARCH_X86_64)
	PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
#elif defined(__aarch64__)
	PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();
#endif
	// guest address 0 also serves as the fallback entry
	PPCRecompiler_allocateRange(0, 0x1000);
	PPCRecompiler_allocateRange(mmuRange_TRAMPOLINE_AREA.getBase(), mmuRange_TRAMPOLINE_AREA.getSize());
	PPCRecompiler_allocateRange(mmuRange_CODECAVE.getBase(), mmuRange_CODECAVE.getSize());

	cemuLog_log(LogType::Force, "Recompiler initialized");
	s_rec.initialized = true;
	s_rec.enableCount = 1;
	s_rec.stopWorker = false;
	s_rec.worker = std::thread(CompilerThread);
}

void PPCRecompiler_Shutdown()
{
	s_rec.stopWorker = true;
	if (s_rec.worker.joinable())
		s_rec.worker.join();
	std::queue<uint32>().swap(s_rec.requests);
	s_rec.invalidatedDuringCompile.clear();
	{
		uint32 rStart, rEnd;
		PPCRecCompiledFunction* func;
		while (s_rec.functions.findFirstRange(0, 0xFFFFFFFF, rStart, rEnd, func))
			DeleteFunction(func);
	}
	s_rec.functions.clear();
	if (ppcRecompilerInstanceData)
	{
		for (uint32 i = 0; i < kTableStepCount; i++)
		{
			if (!s_rec.tableStepCommitted[i])
				continue;
			MemMapper::FreeMemory(JumpTable() + (size_t)i * (kTableStep / 4), (kTableStep / 4) * sizeof(PPCREC_JUMP_ENTRY), true);
			s_rec.tableStepCommitted[i] = false;
		}
	}
	s_rec.enableCount = 0;
	s_rec.initialized = false;
}

void PPCRecompiler_Enable()
{
	if (s_rec.initialized)
		s_rec.enableCount++;
}

void PPCRecompiler_Disable()
{
	if (s_rec.initialized)
		s_rec.enableCount--;
}
