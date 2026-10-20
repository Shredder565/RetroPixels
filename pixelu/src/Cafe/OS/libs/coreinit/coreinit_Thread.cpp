// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/HW/Espresso/Debugger/GDBStub.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"

#include "util/helpers/Semaphore.h"
#include "util/helpers/ConcurrentQueue.h"
#include "util/Fiber/Fiber.h"
#include "util/helpers/helpers.h"

#ifdef __arm64__
#if defined(__clang__)
#include <arm_acle.h>
#elif defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

#if BOOST_OS_LINUX
#include <unistd.h>
#include <sys/prctl.h>
#endif

// Guest threads and the scheduler.
//
// Every guest thread owns a host fiber with its own PPC register file. Host "core" threads (one in
// single core mode, three otherwise) run an idle fiber that picks the best ready thread from the
// core's run queue and switches to that thread's fiber. A thread runs until its timeslice is used
// up or it blocks, then switches back through __OSThreadSwitchToNext. The scheduler lock is held
// for every switch and released by whichever fiber resumes.

SlimRWLock srwlock_activeThreadList;

// all activated guest threads, for the debugger views
MPTR activeThread[256];
sint32 activeThreadCount = 0;

void nnNfp_update();

namespace
{
	// flush denormals to zero like the console; on arm64 keep NaN payloads (FZ=1, DN=0)
	void SetHostFloatMode()
	{
#if defined(ARCH_X86_64)
		_mm_setcsr(_mm_getcsr() | 0x8000);
#elif defined(__arm64__)
		constexpr uint64 kFlushToZero = 1ull << 24;
		constexpr uint64 kDefaultNaN = 1ull << 25;
#if defined(__clang__)
		__arm_wsr64("fpcr", (__arm_rsr64("fpcr") | kFlushToZero) & ~kDefaultNaN);
#elif defined(__GNUC__)
		__builtin_aarch64_set_fpcr((__builtin_aarch64_get_fpcr() | kFlushToZero) & ~kDefaultNaN);
#elif defined(_MSC_VER)
		_WriteStatusReg(ARM64_FPCR, (_ReadStatusReg(ARM64_FPCR) | kFlushToZero) & ~kDefaultNaN);
#endif
#endif
	}
}

namespace coreinit
{
	void __OSRemoveThreadFromRunQueues(OSThread_t* thread);

	/* scheduler state */

	std::atomic<bool> s_schedulerRunning;
	std::mutex s_schedulerStartStopMutex;
	std::vector<std::thread> s_coreHostThreads;

	// every thread that was activated and not yet released, linked through activeThreadChain
	SysAllocator<OSThreadQueue> s_activeThreads;
	// ready threads per core, linked through linkRun[core]
	SysAllocator<OSThreadQueue, 3> s_runQueue;
	// number of threads in each run queue; lets idle cores sleep
	CounterSemaphore s_runQueueSize[3];

	bool s_multiCoreMode;

	thread_local uint32 t_hostCoreIndex;
	thread_local Fiber* t_hostThreadFiber; // the fiber of the host thread itself, switched to on shutdown

	Fiber* s_coreIdleFiber[3]{};

	OSThread_t* s_runningThread[3] = {};

	bool __CemuIsMulticoreMode()
	{
		return s_multiCoreMode;
	}

	/* host fibers */

#ifdef __arm64__
	void GuestFiberMain(uint32 ptrHigh, uint32 ptrLow);
#else
	void GuestFiberMain(void* param);
#endif

	struct GuestFiber
	{
		explicit GuestFiber(OSThread_t* thread) : guestThread(thread), fiber((void (*)(void*))GuestFiberMain, this, this) {}

		OSThread_t* guestThread;
		Fiber fiber;
		// the recompiler uses the memory in front of the register file as scratch stack space
		uint8 recompilerScratch[128 * 1024];
		PPCInterpreter_t cpu;
		uint32 coreToRunOn;
	};

	std::unordered_map<OSThread_t*, GuestFiber*> s_guestFibers;

	void CreateGuestFiber(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		cemu_assert_debug(!s_guestFibers.contains(thread));
		s_guestFibers.emplace(thread, new GuestFiber(thread));
	}

	void ReleaseGuestFiber(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		// the fiber being released may be the one executing this code, so it is freed only when the
		// next one is released
		static GuestFiber* s_retiredFiber = nullptr;
		delete s_retiredFiber;
		s_retiredFiber = nullptr;
		auto it = s_guestFibers.find(thread);
		if (it == s_guestFibers.end())
			return;
		s_retiredFiber = it->second;
		s_guestFibers.erase(it);
	}

	/* active thread list */

	void MarkThreadActive(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		s_activeThreads->addThread(thread, &thread->activeThreadChain);
		MPTR threadAddr = memory_getVirtualOffsetFromPointer(thread);
		srwlock_activeThreadList.LockWrite();
		bool listed = std::find(activeThread, activeThread + activeThreadCount, threadAddr) != activeThread + activeThreadCount;
		cemu_assert_debug(!listed);
		if (!listed)
			activeThread[activeThreadCount++] = threadAddr;
		CreateGuestFiber(thread);
		srwlock_activeThreadList.UnlockWrite();
	}

	// the thread must already be in STATE_NONE
	void MarkThreadInactive(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		MPTR threadAddr = memory_getVirtualOffsetFromPointer(thread);
		srwlock_activeThreadList.LockWrite();
		bool found = false;
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			if (activeThread[i] != threadAddr)
				continue;
			activeThreadCount--;
			activeThread[i] = activeThread[activeThreadCount];
			found = true;
			break;
		}
		cemu_assert_debug(found);
		srwlock_activeThreadList.UnlockWrite();
		s_activeThreads->removeThread(thread, &thread->activeThreadChain);
		cemu_assert_debug(thread->state == OSThread_t::THREAD_STATE::STATE_NONE);
		thread->id = 0x8000;
		ReleaseGuestFiber(thread);
	}

	bool __OSIsThreadActive(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		MPTR threadAddr = memory_getVirtualOffsetFromPointer(thread);
		srwlock_activeThreadList.LockWrite();
		bool found = std::find(activeThread, activeThread + activeThreadCount, threadAddr) != activeThread + activeThreadCount;
		srwlock_activeThreadList.UnlockWrite();
		return found;
	}

	void __OSDeleteAllActivePPCThreads()
	{
		__OSLockScheduler();
		while (activeThreadCount > 0)
		{
			OSThread_t* thread = MEMPTR<OSThread_t>(activeThread[0]).GetPtr();
			thread->state = OSThread_t::THREAD_STATE::STATE_NONE;
			MarkThreadInactive(thread);
		}
		__OSUnlockScheduler();
	}

	/* current thread */

	void OSSetCurrentThread(uint32 coreIndex, OSThread_t* thread)
	{
		if (coreIndex < 3)
			s_runningThread[coreIndex] = thread;
	}

	OSThread_t* OSGetCurrentThread()
	{
		PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
		if (!cpu)
			return nullptr;
		return s_runningThread[cpu->spr.UPIR];
	}

	/* thread setup */

	// first code a new thread runs: runs the C++ runtime's per thread exception setup, then jumps
	// to the entry point with the original arguments and return address
	void GuestThreadStart(PPCInterpreter_t* hCPU)
	{
		OSThread_t* self = OSGetCurrentThread();
		uint32 savedR3 = hCPU->gpr[3];
		uint32 savedR4 = hCPU->gpr[4];
		uint32 savedLR = hCPU->spr.LR;
		// the console disables interrupts around this call, but some titles yield inside it
		// (Angry Birds Star Wars), so interrupts stay enabled
		MPTR ehInit = _swapEndianU32(gCoreinitData->__cpp_exception_init_ptr);
		if (ehInit != MPTR_NULL)
			PPCCoreCallback(ehInit, &self->crt.eh_globals);
		hCPU->gpr[3] = savedR3;
		hCPU->gpr[4] = savedR4;
		hCPU->spr.LR = savedLR;
		hCPU->instructionPointer = self->entrypoint.GetMPTR();
	}

	// return address of every thread entry point
	void GuestThreadReturn(PPCInterpreter_t* hCPU)
	{
		ppcDefineParamU32(exitValue, 0);
		OSExitThread(exitValue);
	}

	void ResetContext(OSContext_t& ctx, MPTR startAddress, MPTR stackPointer)
	{
		ctx.SetContextMagic();
		for (uint32& reg : ctx.gpr)
			reg = 0;
		ctx.gpr[1] = _swapEndianU32(stackPointer);
		ctx.gpr[2] = _swapEndianU32(RPLLoader_GetSDA2Base());
		ctx.gpr[13] = _swapEndianU32(RPLLoader_GetSDA1Base());
		ctx.srr0 = startAddress;
		ctx.srr1 = 0x9032;
		ctx.cr = 0;
		ctx.xer = 0;
		ctx.dsi_dsisr = 0;
		ctx.dsi_dar = 0;
		ctx.ukn0A8 = 0;
		ctx.ukn0AC = 0;
		for (uint32& gqr : ctx.gqr)
			gqr = 0;
		ctx.upir = 0;
		ctx.boostCount = 0;
		ctx.state = 0;
		for (auto& t : ctx.coretime)
			t = 0;
		ctx.starttime = 0;
		ctx.ghs_errno = 0;
		ctx.upmc1 = 0;
		ctx.upmc2 = 0;
		ctx.upmc3 = 0;
		ctx.upmc4 = 0;
		ctx.ummcr0 = 0;
		ctx.ummcr1 = 0;
	}

	void SetupThreadObject(OSThread_t* thread, MEMPTR<void> entry, uint32 arg0, MEMPTR<void> arg1, MEMPTR<void> stackTop, uint32 stackSize, sint32 priority, uint32 coreIndex, OSThread_t::THREAD_TYPE type)
	{
		thread->SetThreadMagic();
		thread->type = type;
		thread->basePriority = priority;
		thread->effectivePriority = priority;
		thread->id = 0x8000;
		thread->entrypoint = entry;
		thread->waitAlarm = nullptr;
		bool hasEntry = !entry.IsNull();
		thread->state = hasEntry ? OSThread_t::THREAD_STATE::STATE_READY : OSThread_t::THREAD_STATE::STATE_NONE;
		thread->suspendCounter = hasEntry ? 1 : 0; // started threads begin suspended until OSResumeThread
		thread->exitValue = 0xFFFFFFFF;
		thread->requestFlags = OSThread_t::REQUEST_FLAG_NONE;
		thread->pendingSuspend = 0;
		thread->suspendResult = -1;
		thread->deallocatorFunc = nullptr;
		thread->cleanupCallback = nullptr;
		thread->waitingForMutex = nullptr;
		thread->waitingForFastMutex = nullptr;
		thread->stateFlags = 0;
		memset(&thread->crt, 0, sizeof(crt_t));
		thread->tlsBlocksMPTR = MPTR_NULL;
		thread->numAllocatedTLSBlocks = 0;
		thread->tlsStatus = 0;

		OSInitThreadQueueEx(&thread->joinQueue, thread);
		OSInitThreadQueueEx(&thread->suspendQueue, thread);
		thread->mutexQueue.head = nullptr;
		thread->mutexQueue.tail = nullptr;
		thread->mutexQueue.ukn08 = thread;
		thread->mutexQueue.ukn0C = 0;
		thread->ownedFastMutex.next = nullptr;
		thread->ownedFastMutex.prev = nullptr;
		thread->contendedFastMutex.next = nullptr;
		thread->contendedFastMutex.prev = nullptr;

		// two zero words (back chain and LR save slot) at the 8 byte aligned top of the stack
		MPTR stackTopAddr = stackTop.GetMPTR();
		MPTR initialSP = (stackTopAddr & ~7u) - 8;
		*MEMPTR<uint32be>(initialSP + 4) = 0;
		*MEMPTR<uint32be>(initialSP) = 0;

		ResetContext(thread->context, PPCInterpreter_makeCallableExportDepr(GuestThreadStart), initialSP);
		thread->stackBase = stackTop;
		thread->stackEnd = MEMPTR<void>(stackTopAddr - stackSize);
		thread->context.upir = coreIndex;
		thread->context.lr = _swapEndianU32(PPCInterpreter_makeCallableExportDepr(GuestThreadReturn));
		thread->context.gpr[3] = _swapEndianU32(arg0);
		thread->context.gpr[4] = _swapEndianU32(arg1.GetMPTR());
		// stack overflow canary at the lowest address
		*MEMPTR<uint32be>(stackTopAddr - stackSize) = 0xDEADBABE;

		thread->alarmRelatedUkn = 0;
		for (auto& slot : thread->specificArray)
			slot = nullptr;

		thread->context.fpscr.fpscr = 4;
		for (sint32 i = 0; i < 32; i++)
		{
			thread->context.fp_ps0[i] = 0;
			thread->context.fp_ps1[i] = 0;
		}
		// default paired single quantization: types 4-7 in GQR2-5
		thread->context.gqr[2] = 0x40004;
		thread->context.gqr[3] = 0x50005;
		thread->context.gqr[4] = 0x60006;
		thread->context.gqr[5] = 0x70007;
		for (auto& t : thread->context.coretime)
			t = 0;

		// the console leaves these as they were; clearing them keeps the queue asserts meaningful
		for (auto& q : thread->currentRunQueue)
			q = nullptr;
		thread->waitQueueLink.next = nullptr;
		thread->waitQueueLink.prev = nullptr;

		thread->quantumTicks = 0;
		thread->coretimeSumQuantumStart = 0;
		thread->wakeUpCount = 0;
		thread->wakeUpTime = 0;
		thread->totalCycles = 0;
		thread->wakeTimeRelatedUkn1 = 0x7FFFFFFFFFFFFFFF;
		thread->wakeTimeRelatedUkn2 = 0;
		for (auto& w : thread->padding68C)
			w = 0;
	}

	void BindThreadToCore(OSThread_t* thread, uint32 coreIndex)
	{
		cemu_assert_debug(coreIndex < 3);
		uint32 bit = 1u << std::min<uint32>(coreIndex, 2);
		thread->attr = (thread->attr & ~(OSThread_t::ATTR_AFFINITY_CORE0 | OSThread_t::ATTR_AFFINITY_CORE1 | OSThread_t::ATTR_AFFINITY_CORE2 | OSThread_t::ATTR_UKN_010)) | bit;
		thread->context.affinity = (thread->context.affinity & ~7u) | bit;
	}

	// Some titles (Fast Racing Neo) call OSCreateThread on a thread object that has not finished
	// yet, or that finished without being detached. Wait for it to stop and release it first.
	void WaitForReusedThreadObject(OSThread_t* thread)
	{
		__OSLockScheduler();
		if (__OSIsThreadActive(thread))
		{
			cemuLog_log(LogType::Force, "OSCreateThread: thread object 0x{:08x} is still in use, waiting for it to end", MEMPTR<OSThread_t>(thread).GetMPTR());
			auto stillRunnable = [thread]() {
				return (thread->state == OSThread_t::THREAD_STATE::STATE_READY || thread->state == OSThread_t::THREAD_STATE::STATE_RUNNING) && thread->suspendCounter == 0;
			};
			while (stillRunnable())
			{
				__OSUnlockScheduler();
				OSSleepTicks(ESPRESSO_TIMER_CLOCK / 2000); // 0.5ms
				__OSLockScheduler();
			}
			if (__OSIsThreadActive(thread) && thread->state == OSThread_t::THREAD_STATE::STATE_MORIBUND)
			{
				cemuLog_log(LogType::Force, "OSCreateThread: thread object ended without being detached or joined, detaching it");
				__OSUnlockScheduler();
				OSDetachThread(thread);
				__OSLockScheduler();
			}
		}
		cemu_assert_debug(!__OSIsThreadActive(thread));
		__OSUnlockScheduler();
	}

	bool CreateGuestThread(OSThread_t* thread, MEMPTR<void> entry, uint32 arg0, MEMPTR<void> arg1, MEMPTR<void> stackTop, uint32 stackSize, sint32 priority, uint32 attr, OSThread_t::THREAD_TYPE type)
	{
		WaitForReusedThreadObject(thread);
		OSThread_t* creator = OSGetCurrentThread();
		if (priority < 0 || priority > 31)
		{
			cemuLog_log(LogType::APIErrors, "OSCreateThread: priority {} is outside of 0-31", priority);
			return false;
		}
		priority += OSThread_t::GetTypePriorityBase(type);
		if (attr >= 0x20 || stackTop.IsNull() || stackSize == 0)
		{
			cemuLog_logDebug(LogType::APIErrors, "OSCreateThread: bad attributes or stack");
			return false;
		}

		uint32 prevInterrupts = OSDisableInterrupts();
		__OSLockScheduler(thread);

		uint32 creatorCore = PPCInterpreter_getCurrentInstance() ? OSGetCoreId() : 1;
		SetupThreadObject(thread, entry, arg0, arg1, stackTop, stackSize, priority, creatorCore, type);
		thread->threadName = nullptr;
		thread->attr = attr;
		thread->context.affinity = attr & 7;
		if ((attr & 7) == 0)
			BindThreadToCore(thread, OSGetCoreId()); // no affinity given: run where the creator runs

		// exception handlers and some machine state are inherited from the creator
		for (sint32 i = 0; i < Espresso::CORE_COUNT; i++)
		{
			thread->dsiCallback[i] = creator ? creator->dsiCallback[i] : MPTR_NULL;
			thread->isiCallback[i] = creator ? creator->isiCallback[i] : MPTR_NULL;
			thread->programCallback[i] = creator ? creator->programCallback[i] : MPTR_NULL;
			thread->perfMonCallback[i] = creator ? creator->perfMonCallback[i] : MPTR_NULL;
			thread->alignmentExceptionCallback[i] = creator ? creator->alignmentExceptionCallback[i] : nullptr;
		}
		if (creator)
		{
			thread->context.srr1 |= creator->context.srr1 & 0x900;
			thread->context.fpscr.fpscr = thread->context.fpscr.fpscr | (creator->context.fpscr.fpscr & 0xF8);
		}

		if (!entry.IsNull())
		{
			thread->id = 0x8000;
			MarkThreadActive(thread);
		}
		__OSUnlockScheduler(thread);
		OSRestoreInterrupts(prevInterrupts);
		if (!entry.IsNull())
			PPCRecompiler_recompileIfUnvisited(entry.GetMPTR());
		return true;
	}

	bool OSCreateThreadType(OSThread_t* thread, MPTR entryPoint, sint32 numParam, void* ptrParam, void* stackTop, sint32 stackSize, sint32 priority, uint32 attr, OSThread_t::THREAD_TYPE threadType)
	{
		// the public variant only creates application and I/O threads
		if (threadType != OSThread_t::THREAD_TYPE::TYPE_APP && threadType != OSThread_t::THREAD_TYPE::TYPE_IO)
		{
			cemuLog_logDebug(LogType::APIErrors, "OSCreateThreadType: thread type {} is not allowed", (uint32)threadType);
			return false;
		}
		return CreateGuestThread(thread, MEMPTR<void>(entryPoint), numParam, ptrParam, stackTop, stackSize, priority, attr, threadType);
	}

	bool OSCreateThread(OSThread_t* thread, MPTR entryPoint, sint32 numParam, void* ptrParam, void* stackTop, sint32 stackSize, sint32 priority, uint32 attr)
	{
		return CreateGuestThread(thread, MEMPTR<void>(entryPoint), numParam, ptrParam, stackTop, stackSize, priority, attr, OSThread_t::THREAD_TYPE::TYPE_APP);
	}

	bool __OSCreateThreadType(OSThread_t* thread, MPTR entryPoint, sint32 numParam, void* ptrParam, void* stackTop, sint32 stackSize, sint32 priority, uint32 attr, OSThread_t::THREAD_TYPE threadType)
	{
		return CreateGuestThread(thread, MEMPTR<void>(entryPoint), numParam, ptrParam, stackTop, stackSize, priority, attr, threadType);
	}

	// restarts a thread object that is not running with a new function
	bool OSRunThread(OSThread_t* thread, MPTR funcAddress, sint32 numParam, void* ptrParam)
	{
		__OSLockScheduler();
		cemu_assert_debug(!PPCInterpreter_getCurrentInstance() || OSGetCurrentThread() != thread);
		if (thread->state == OSThread_t::THREAD_STATE::STATE_MORIBUND)
		{
			thread->state = OSThread_t::THREAD_STATE::STATE_NONE;
			MarkThreadInactive(thread);
			__OSRemoveThreadFromRunQueues(thread);
		}
		else if (thread->state != OSThread_t::THREAD_STATE::STATE_NONE)
		{
			cemuLog_logDebug(LogType::Force, "OSRunThread: thread 0x{:08x} is still running", MEMPTR<OSThread_t>(thread).GetMPTR());
			__OSUnlockScheduler();
			return false;
		}
		thread->entrypoint = funcAddress;
		thread->context.srr0 = PPCInterpreter_makeCallableExportDepr(GuestThreadStart);
		thread->context.lr = _swapEndianU32(PPCInterpreter_makeCallableExportDepr(GuestThreadReturn));
		thread->context.gpr[3] = _swapEndianU32((uint32)numParam);
		thread->context.gpr[4] = _swapEndianU32(memory_getVirtualOffsetFromPointer(ptrParam));
		thread->suspendCounter = 0;
		MarkThreadActive(thread);
		thread->state = OSThread_t::THREAD_STATE::STATE_READY;
		__OSAddReadyThreadToRunQueue(thread);
		__OSUnlockScheduler();
		return true;
	}

	/* thread end */

	void OSExitThread(sint32 exitValue)
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		hCPU->gpr[3] = exitValue;
		OSThread_t* self = OSGetCurrentThread();

		if (!self->cleanupCallback.IsNull())
		{
			// flag 1 marks the thread as running its cleanup callback (the field is big endian)
			self->stateFlags = _swapEndianU32(_swapEndianU32(self->stateFlags) | 1);
			PPCCoreCallback(self->cleanupCallback.GetMPTR(), self, self->stackEnd);
		}
		MPTR ehCleanup = _swapEndianU32(gCoreinitData->__cpp_exception_cleanup_ptr);
		if (ehCleanup != MPTR_NULL && !self->crt.eh_globals.IsNull())
		{
			PPCCoreCallback(ehCleanup, &self->crt.eh_globals);
			self->crt.eh_globals = nullptr;
		}
		self->exitValue = exitValue;

		__OSLockScheduler();
		if (!self->mutexQueue.isEmpty())
		{
			cemuLog_log(LogType::Force, "OSExitThread: thread still holds mutexes, releasing them");
			while (OSMutex* mutex = self->mutexQueue.getFirst())
			{
				if (mutex->owner == self)
				{
					OSUnlockMutexInternal(mutex);
					continue;
				}
				cemuLog_log(LogType::Force, "OSExitThread: held mutex list contains a mutex owned by another thread");
				self->mutexQueue.removeMutex(mutex);
			}
		}
		if (!self->joinQueue.isEmpty())
			self->joinQueue.wakeupEntireWaitQueue(false);

		if ((self->attr & OSThread_t::ATTR_DETACHED) != 0)
		{
			// nobody will join a detached thread, release it right away
			self->state = OSThread_t::THREAD_STATE::STATE_NONE;
			MarkThreadInactive(self);
			if (!self->deallocatorFunc.IsNull())
				__OSQueueThreadDeallocation(self);
		}
		else
		{
			self->state = OSThread_t::THREAD_STATE::STATE_MORIBUND;
		}
		PPCCore_switchToSchedulerWithLock();
	}

	void OSDetachThread(OSThread_t* thread)
	{
		__OSLockScheduler();
		thread->attr |= OSThread_t::ATTR_DETACHED;
		if (thread->state == OSThread_t::THREAD_STATE::STATE_MORIBUND)
		{
			thread->state = OSThread_t::THREAD_STATE::STATE_NONE;
			thread->id = 0x8000;
			MarkThreadInactive(thread);
			if (!thread->joinQueue.isEmpty())
				thread->joinQueue.wakeupEntireWaitQueue(true);
		}
		__OSUnlockScheduler();
	}

	bool OSJoinThread(OSThread_t* thread, uint32be* exitValue)
	{
		__OSLockScheduler();
		if (thread->state != OSThread_t::THREAD_STATE::STATE_MORIBUND)
		{
			if ((thread->attr & OSThread_t::ATTR_DETACHED) != 0)
			{
				cemuLog_logDebug(LogType::Force, "OSJoinThread: thread is detached");
				__OSUnlockScheduler();
				return false;
			}
			cemu_assert_debug(thread->joinQueue.isEmpty());
			thread->joinQueue.queueAndWait(OSGetCurrentThread());
		}
		cemu_assert_debug(thread->state == OSThread_t::THREAD_STATE::STATE_MORIBUND);
		if (exitValue)
			*exitValue = thread->exitValue;
		thread->state = OSThread_t::THREAD_STATE::STATE_NONE;
		MarkThreadInactive(thread);
		__OSRemoveThreadFromRunQueues(thread);
		thread->id = 0x8000;
		if (!thread->deallocatorFunc.IsNull())
		{
			__OSQueueThreadDeallocation(thread);
			// let the terminator thread run the deallocator before returning
			PPCCore_switchToSchedulerWithLock();
		}
		__OSUnlockScheduler();
		return true;
	}

	/* simple attributes */

	void OSSetThreadSpecific(uint32 index, void* value)
	{
		OSThread_t* self = OSGetCurrentThread();
		if (index < self->specificArray.size())
			self->specificArray[index] = value;
	}

	void* OSGetThreadSpecific(uint32 index)
	{
		OSThread_t* self = OSGetCurrentThread();
		if (index < self->specificArray.size())
			return self->specificArray[index].GetPtr();
		return nullptr;
	}

	void OSSetThreadName(OSThread_t* thread, const char* name)
	{
		thread->threadName = name;
	}

	const char* OSGetThreadName(OSThread_t* thread)
	{
		return thread->threadName.GetPtr();
	}

	void OSYieldThread()
	{
		PPCCore_switchToScheduler();
	}

	void WakeSleepQueue(uint64 currentTick, void* context)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		((OSThreadQueue*)context)->wakeupEntireWaitQueue(false);
	}

	void OSSleepTicks(uint64 ticks)
	{
		cemu_assert_debug(!__OSHasSchedulerLock());
		StackAllocator<OSThreadQueue> sleepQueue;
		OSInitThreadQueue(sleepQueue.GetPointer());
		__OSLockScheduler();
		OSHostAlarm* wakeAlarm = OSHostAlarmCreate(OSGetTime() + ticks, 0, WakeSleepQueue, sleepQueue.GetPointer());
		sleepQueue->queueAndWait(OSGetCurrentThread());
		OSHostAlarmDestroy(wakeAlarm);
		__OSUnlockScheduler();
	}

	bool OSSetThreadAffinity(OSThread_t* thread, uint32 affinityMask)
	{
		cemu_assert_debug((affinityMask & ~7u) == 0);
		__OSLockScheduler();
		auto apply = [thread, affinityMask]() {
			thread->attr = (thread->attr & ~7) | (affinityMask & 7);
			thread->context.setAffinity(affinityMask);
		};
		bool changed = thread->context.getAffinity() != affinityMask;
		if (thread->state == OSThread_t::THREAD_STATE::STATE_RUNNING)
			apply(); // takes effect when it is next queued
		else if (changed && thread->state != OSThread_t::THREAD_STATE::STATE_NONE)
		{
			__OSRemoveThreadFromRunQueues(thread);
			apply();
			__OSAddReadyThreadToRunQueue(thread);
		}
		else if (changed)
			apply();
		__OSUnlockScheduler();
		return true;
	}

	uint32 OSGetThreadAffinity(OSThread_t* thread)
	{
		uint32 mask = thread->context.getAffinity();
		cemu_assert_debug((mask & ~7u) == 0);
		return mask;
	}

	void* OSSetThreadDeallocator(OSThread_t* thread, void* newDeallocatorFunc)
	{
		__OSLockScheduler();
		void* previous = thread->deallocatorFunc.GetPtr();
		thread->deallocatorFunc = newDeallocatorFunc;
		__OSUnlockScheduler();
		return previous;
	}

	void* OSSetThreadCleanupCallback(OSThread_t* thread, void* cleanupCallback)
	{
		__OSLockScheduler();
		void* previous = thread->cleanupCallback.GetPtr();
		thread->cleanupCallback = cleanupCallback;
		__OSUnlockScheduler();
		return previous;
	}

	/* priority */

	void __OSSetThreadBasePriority(OSThread_t* thread, sint32 newPriority)
	{
		cemu_assert_debug(newPriority >= 0 && newPriority <= 31);
		thread->basePriority = newPriority + OSThread_t::GetTypePriorityBase(thread->type);
	}

	void __OSUpdateThreadEffectivePriority(OSThread_t* thread)
	{
		// a boosted thread (spinlock holder) runs at the highest priority
		thread->effectivePriority = thread->context.boostCount != 0 ? 0 : (sint32)thread->basePriority;
	}

	bool OSSetThreadPriority(OSThread_t* thread, sint32 newPriority)
	{
		if (newPriority < 0 || newPriority > 31)
		{
			cemuLog_logDebug(LogType::APIErrors, "OSSetThreadPriority: priority {} is outside of 0-31", newPriority);
			return false;
		}
		__OSLockScheduler();
		__OSSetThreadBasePriority(thread, newPriority);
		__OSUpdateThreadEffectivePriority(thread);
		OSThread_t* self = OSGetCurrentThread();
		if (self && self != thread && __OSCoreShouldSwitchToThread(self, thread, false))
			PPCCore_switchToSchedulerWithLock();
		__OSUnlockScheduler();
		return true;
	}

	sint32 OSGetThreadPriority(OSThread_t* thread)
	{
		sint32 priority = (sint32)thread->basePriority - OSThread_t::GetTypePriorityBase(thread->type);
		cemu_assert_debug(priority >= 0 && priority <= 31);
		return priority;
	}

	/* state queries */

	bool OSIsThreadTerminated(OSThread_t* thread)
	{
		__OSLockScheduler();
		OSThread_t::THREAD_STATE state = thread->state;
		__OSUnlockScheduler();
		return state == OSThread_t::THREAD_STATE::STATE_NONE || state == OSThread_t::THREAD_STATE::STATE_MORIBUND;
	}

	bool OSIsThreadSuspended(OSThread_t* thread)
	{
		__OSLockScheduler();
		sint32 count = thread->suspendCounter;
		__OSUnlockScheduler();
		return count > 0;
	}

	bool OSIsThreadRunningNoLock(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		return thread->state == OSThread_t::THREAD_STATE::STATE_RUNNING;
	}

	bool OSIsThreadRunning(OSThread_t* thread)
	{
		__OSLockScheduler();
		bool running = OSIsThreadRunningNoLock(thread);
		__OSUnlockScheduler();
		return running;
	}

	/* cancellation */

	void OSCancelThread(OSThread_t* thread)
	{
		__OSLockScheduler();
		thread->requestFlags = OSThread_t::REQUEST_FLAG_CANCEL;
		__OSUnlockScheduler();
		if (thread == OSGetCurrentThread())
			OSExitThread(-1);
	}

	// scheduler lock must be held; does not return if the current thread was cancelled
	void OSTestThreadCancelInternal()
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		if (OSGetCurrentThread()->requestFlags == OSThread_t::REQUEST_FLAG_CANCEL)
		{
			__OSUnlockScheduler();
			OSExitThread(-1);
		}
	}

	void OSTestThreadCancel()
	{
		__OSLockScheduler();
		OSTestThreadCancelInternal();
		__OSUnlockScheduler();
	}

	/* run queues */

	// queue a ready, unsuspended thread on every core its affinity allows
	void __OSAddReadyThreadToRunQueue(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		cemu_assert_debug(thread->IsValidMagic());
		if (thread->state != OSThread_t::THREAD_STATE::STATE_READY || thread->suspendCounter != 0)
			return;
		for (uint32 core = 0; core < 3; core++)
		{
			if (!thread->currentRunQueue[core].IsNull() || !thread->context.hasCoreAffinitySet(core))
				continue;
			OSThreadQueue* queue = s_runQueue.GetPtr() + core;
			queue->addThread(thread, &thread->linkRun[core]);
			thread->currentRunQueue[core] = queue;
			s_runQueueSize[core].increment();
		}
	}

	void __OSRemoveThreadFromRunQueues(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		for (uint32 core = 0; core < 3; core++)
		{
			if (thread->currentRunQueue[core].IsNull())
				continue;
			s_runQueue.GetPtr()[core].removeThread(thread, &thread->linkRun[core]);
			thread->currentRunQueue[core] = nullptr;
			s_runQueueSize[core].decrement();
		}
	}

	// whether the calling core should give up its thread for newThread
	bool __OSCoreShouldSwitchToThread(OSThread_t* currentThread, OSThread_t* newThread, bool sharedPriorityAndAffinityWorkaround)
	{
		uint32 core = OSGetCoreId();
		if (!newThread->context.hasCoreAffinitySet(core))
			return false;
		// Two threads pinned to this same core at equal priority: hand over anyway. Without this a
		// thread in Just Dance 2019 keeps re-taking a mutex within its timeslice and starves the other.
		if (sharedPriorityAndAffinityWorkaround)
		{
			uint32 newAffinity = newThread->context.affinity;
			if (newAffinity == (1u << core) && (uint32)currentThread->context.affinity == newAffinity && currentThread->effectivePriority == newThread->effectivePriority)
				return true;
		}
		return newThread->effectivePriority < currentThread->effectivePriority;
	}

	/* suspend and resume */

	sint32 __OSResumeThreadInternal(OSThread_t* thread, sint32 resumeCount)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		sint32 previous = thread->suspendCounter;
		if (previous == 0)
		{
			cemuLog_log(LogType::APIErrors, "OSResumeThread: thread 0x{:08x} is not suspended", MEMPTR<OSThread_t>(thread).GetMPTR());
			return 0;
		}
		sint32 remaining = std::max<sint32>(previous - resumeCount, 0);
		thread->suspendCounter = remaining;
		if (remaining == 0)
		{
			__OSAddReadyThreadToRunQueue(thread);
			thread->wakeUpTime = PPCInterpreter_getMainCoreCycleCounter();
			if (PPCInterpreter_getCurrentInstance() && __OSCoreShouldSwitchToThread(OSGetCurrentThread(), thread, false))
				PPCCore_switchToSchedulerWithLock();
		}
		return previous;
	}

	sint32 OSResumeThread(OSThread_t* thread)
	{
		__OSLockScheduler();
		sint32 previous = __OSResumeThreadInternal(thread, 1);
		__OSUnlockScheduler();
		return previous;
	}

	void OSContinueThread(OSThread_t* thread)
	{
		__OSLockScheduler();
		__OSResumeThreadInternal(thread, thread->suspendCounter);
		__OSUnlockScheduler();
	}

	void __OSSuspendThreadInternal(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		cemu_assert_debug(thread->state != OSThread_t::THREAD_STATE::STATE_NONE && thread->state != OSThread_t::THREAD_STATE::STATE_MORIBUND);
		sint32 previous = thread->suspendCounter;
		thread->suspendCounter = previous + 1;
		if (thread == OSGetCurrentThread())
		{
			// the store on switch-out sees the counter and keeps the thread off the run queues
			PPCCore_switchToSchedulerWithLock();
		}
		else if (previous == 0)
		{
			// a thread running on another core keeps going until its timeslice ends
			__OSRemoveThreadFromRunQueues(thread);
		}
	}

	// caller holds the scheduler lock
	void __OSSuspendThreadNolock(OSThread_t* thread)
	{
		__OSSuspendThreadInternal(thread);
	}

	void OSSuspendThread(OSThread_t* thread)
	{
		__OSLockScheduler();
		__OSSuspendThreadNolock(thread);
		__OSUnlockScheduler();
	}

	void OSWakeupThread(OSThreadQueue* threadQueue)
	{
		__OSLockScheduler();
		threadQueue->wakeupEntireWaitQueue(true);
		__OSUnlockScheduler();
	}

	void OSSleepThread(OSThreadQueue* threadQueue)
	{
		OSThread_t* const self = OSGetCurrentThread();
		__OSLockScheduler();
		threadQueue->queueAndWait(self);
		__OSUnlockScheduler();
	}

	/* register state */

	void RestoreRegisters(PPCInterpreter_t* cpu, OSThread_t* thread)
	{
		OSContext_t& ctx = thread->context;
		for (uint32 i = 0; i < 32; i++)
			cpu->gpr[i] = _swapEndianU32(ctx.gpr[i]);
		ppc_setCR(cpu, ctx.cr);
		cpu->spr.LR = _swapEndianU32(ctx.lr);
		cpu->spr.CTR = ctx.ctr;
		PPCInterpreter_setXER(cpu, ctx.xer);
		cpu->fpscr = ctx.fpscr.fpscr;
		for (uint32 i = 0; i < 32; i++)
		{
			cpu->fpr[i].fp0int = ctx.fp_ps0[i];
			cpu->fpr[i].fp1int = ctx.fp_ps1[i];
		}
		for (uint32 i = 0; i < 8; i++)
			cpu->spr.UGQR[i] = ctx.gqr[i];
		cpu->instructionPointer = ctx.srr0;
	}

	void SaveRegisters(PPCInterpreter_t* cpu, OSThread_t* thread)
	{
		OSContext_t& ctx = thread->context;
		for (uint32 i = 0; i < 32; i++)
			ctx.gpr[i] = _swapEndianU32(cpu->gpr[i]);
		ctx.cr = ppc_getCR(cpu);
		ctx.lr = _swapEndianU32(cpu->spr.LR);
		ctx.ctr = cpu->spr.CTR;
		ctx.xer = PPCInterpreter_getXER(cpu);
		ctx.fpscr.padding = 0;
		ctx.fpscr.fpscr = cpu->fpscr;
		for (uint32 i = 0; i < 32; i++)
		{
			ctx.fp_ps0[i] = cpu->fpr[i].fp0int;
			ctx.fp_ps1[i] = cpu->fpr[i].fp1int;
		}
		for (uint32 i = 0; i < 8; i++)
			ctx.gqr[i] = cpu->spr.UGQR[i];
		ctx.srr0 = cpu->instructionPointer;
	}

	// thread leaves its core: requeue it if it is still runnable and record the cycles it used
	void SwitchOutThread(OSThread_t* thread, PPCInterpreter_t* cpu)
	{
		switch (thread->state)
		{
		case OSThread_t::THREAD_STATE::STATE_RUNNING:
			thread->state = OSThread_t::THREAD_STATE::STATE_READY;
			__OSAddReadyThreadToRunQueue(thread);
			break;
		case OSThread_t::THREAD_STATE::STATE_WAITING:
		case OSThread_t::THREAD_STATE::STATE_MORIBUND:
		case OSThread_t::THREAD_STATE::STATE_NONE:
			break;
		default:
			cemu_assert_debug(false);
			break;
		}
		// pending requests other than cancel are dropped
		thread->requestFlags = (OSThread_t::REQUEST_FLAG_BIT)(thread->requestFlags & OSThread_t::REQUEST_FLAG_CANCEL);

		sint64 used = std::max<sint64>((sint64)(uint64)thread->quantumTicks - (sint64)cpu->remainingCycles, 0);
		sint64 skipped = (sint64)cpu->skippedCycles;
		used = used < skipped ? 0 : used - skipped;
		thread->totalCycles = thread->totalCycles + (uint64)used;

		SaveRegisters(cpu, thread);
		OSSetCurrentThread(OSGetCoreId(), nullptr);
		PPCInterpreter_setCurrentInstance(nullptr);
	}

	void SwitchInThread(OSThread_t* thread, PPCInterpreter_t* cpu, uint32 core)
	{
		cpu->LSQE = 1;
		cpu->PSE = 1;
		cpu->reservedMemAddr = MPTR_NULL;
		cpu->reservedMemValue = 0;
		cpu->spr.UPIR = core;
		cpu->coreInterruptMask = 1;
		PPCInterpreter_setCurrentInstance(cpu);
		OSSetCurrentThread(OSGetCoreId(), thread);
		RestoreRegisters(cpu, thread);
		thread->context.upir = core;
		thread->quantumTicks = ppcThreadQuantum;
		thread->wakeUpTime = PPCInterpreter_getMainCoreCycleCounter();
		thread->wakeUpCount = thread->wakeUpCount + 1;
	}

	// Each timeslice is lengthened by a small pseudo random amount (Park-Miller style generator per
	// core) so that threads cannot settle into a pattern where one is always switched out while it
	// holds a lock (seen in Mario Party 10 at boot).
	uint32 s_timesliceJitter[3] = {};

	void BeginTimeslice(PPCInterpreter_t* cpu)
	{
		uint32 core = PPCInterpreter_getCoreIndex(cpu);
		uint32& state = s_timesliceJitter[core];
		if (state == 0)
			state = 12345;
		cpu->remainingCycles = ppcThreadQuantum + (state & 0x7F);
		cpu->skippedCycles = 0;
		state = (uint32)(((uint64)state * 279470273ull) % 0xFFFFFFFBull);
	}

	// best ready thread of a core's run queue (first one among equals), taken off all run queues
	OSThread_t* TakeNextThread(uint32 core)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		OSThread_t* best = nullptr;
		for (OSThread_t* t = s_runQueue.GetPtr()[core].head.GetPtr(); t; t = t->linkRun[core].next.GetPtr())
		{
			if (!best || t->effectivePriority < best->effectivePriority)
				best = t;
		}
		if (!best)
			return nullptr;
		cemu_assert_debug(best->state == OSThread_t::THREAD_STATE::STATE_READY);
		__OSRemoveThreadFromRunQueues(best);
		best->state = OSThread_t::THREAD_STATE::STATE_RUNNING;
		return best;
	}

	void EnterGuestFiber(OSThread_t* thread, uint32 core)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		auto it = s_guestFibers.find(thread);
		cemu_assert_debug(it != s_guestFibers.end());
		GuestFiber* gf = it->second;
		gf->coreToRunOn = core;
		Fiber::Switch(gf->fiber);
	}

	// periodic emulation work, done by the main core between threads
	void RunPeriodicTasks()
	{
		snd_core::AXOut_update();
		alarm_update();
		nnNfp_update();
	}

	// Idle fiber of a host core thread. Entered with the scheduler lock held. In single core mode
	// one host thread serves all three cores in turn.
	void CoreIdleLoop(void*)
	{
		const bool doesPeriodicTasks = !s_multiCoreMode || t_hostCoreIndex == 1;
		uint32 core = t_hostCoreIndex;
		__OSUnlockScheduler();
		while (true)
		{
			if (!s_runQueueSize[core].isZero())
			{
				__OSLockScheduler();
				if (OSThread_t* next = TakeNextThread(core))
					EnterGuestFiber(next, core); // comes back here when a thread switches to idle
				__OSUnlockScheduler();
			}
			if (doesPeriodicTasks)
			{
				RunPeriodicTasks();
				if (!s_multiCoreMode)
					core = (core + 1) % 3;
			}
			else
			{
				s_runQueueSize[t_hostCoreIndex].waitUntilNonZero();
				if (!s_schedulerRunning.load(std::memory_order_relaxed))
					Fiber::Switch(*t_hostThreadFiber);
			}
		}
	}

	// called with the scheduler lock held by the running guest thread to give up its core; returns
	// (still holding the lock) when the thread is scheduled again
	void __OSThreadSwitchToNext()
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		GuestFiber* self = (GuestFiber*)Fiber::GetFiberPrivateData();
		cemu_assert_debug(self);
		SwitchOutThread(OSGetCurrentThread(), &self->cpu);

		if (!s_schedulerRunning.load(std::memory_order_relaxed))
		{
			__OSUnlockScheduler();
			Fiber::Switch(*t_hostThreadFiber);
		}

		// the main core always goes through the idle loop so the periodic tasks keep running
		const bool isMainCore = !s_multiCoreMode || t_hostCoreIndex == 1;
		OSThread_t* next = isMainCore ? nullptr : TakeNextThread(t_hostCoreIndex);
		if (next)
			EnterGuestFiber(next, t_hostCoreIndex);
		else
			Fiber::Switch(*s_coreIdleFiber[t_hostCoreIndex]);

		cemu_assert_debug(__OSHasSchedulerLock());
		cemu_assert_debug(!s_multiCoreMode || self->coreToRunOn == t_hostCoreIndex);
		SwitchInThread(self->guestThread, &self->cpu, self->coreToRunOn);
		BeginTimeslice(&self->cpu);
	}

#ifdef __arm64__
	void GuestFiberMain(uint32 ptrHigh, uint32 ptrLow)
	{
		GuestFiber* self = (GuestFiber*)(uintptr_t)(((uint64)ptrHigh << 32) | ptrLow);
#else
	void GuestFiberMain(void* param)
	{
		GuestFiber* self = (GuestFiber*)param;
#endif
		SetHostFloatMode();
		PPCInterpreter_t* cpu = &self->cpu;
		SwitchInThread(self->guestThread, cpu, self->coreToRunOn);
		BeginTimeslice(cpu);
		// fibers are always entered with the scheduler lock held
		__OSUnlockScheduler();
		while (true)
		{
			if (cpu->remainingCycles > 0)
			{
				PPCRecompiler_attemptEnterWithoutRecompile(cpu, cpu->instructionPointer);
				while (--cpu->remainingCycles >= 0)
					PPCInterpreterSlim_executeInstruction(cpu);
			}
			cpu->reservedMemAddr = 0;
			cpu->reservedMemValue = 0;
			__OSLockScheduler();
			__OSThreadSwitchToNext();
			__OSUnlockScheduler();
		}
	}

	/* host core threads */

#if BOOST_OS_LINUX
	std::vector<pid_t> s_coreHostThreadIds;
	std::mutex s_coreHostThreadIdsMutex;

	std::vector<pid_t>& OSGetSchedulerThreadIds()
	{
		std::lock_guard guard(s_coreHostThreadIdsMutex);
		return s_coreHostThreadIds;
	}
#endif

	std::vector<std::thread::native_handle_type> s_coreHostThreadHandles;

	std::vector<std::thread::native_handle_type>& OSGetSchedulerThreads()
	{
		return s_coreHostThreadHandles;
	}

	void CoreHostThreadMain(uint32 core)
	{
		SetThreadName(fmt::format("OSSched[core={}]", core).c_str());
		t_hostCoreIndex = core;
		SetHostFloatMode();
#if BOOST_OS_LINUX
		if (g_gdbstub)
		{
			// let the GDB stub attach to this thread
			prctl(PR_SET_DUMPABLE, (unsigned long)1);
			prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY);
		}
		{
			std::lock_guard guard(s_coreHostThreadIdsMutex);
			s_coreHostThreadIds.emplace_back(gettid());
		}
#endif
		t_hostThreadFiber = Fiber::PrepareCurrentThread();
		s_coreIdleFiber[core] = new Fiber(CoreIdleLoop, nullptr, nullptr);
		cemu_assert_debug(!PPCInterpreter_getCurrentInstance());
		__OSLockScheduler();
		Fiber::Switch(*s_coreIdleFiber[core]);
		// back here once the scheduler stops
		cemu_assert_debug(!__OSHasSchedulerLock());
	}

	void OSSchedulerBegin(sint32 numCPUEmulationThreads)
	{
		std::unique_lock guard(s_schedulerStartStopMutex);
		if (s_schedulerRunning.exchange(true))
			return;
		cemu_assert_debug(numCPUEmulationThreads == 1 || numCPUEmulationThreads == 3);
		s_multiCoreMode = numCPUEmulationThreads > 1;
		if (numCPUEmulationThreads == 1)
			s_coreHostThreads.emplace_back(CoreHostThreadMain, 0u);
		else if (numCPUEmulationThreads == 3)
		{
			for (uint32 core = 0; core < 3; core++)
				s_coreHostThreads.emplace_back(CoreHostThreadMain, core);
		}
		for (auto& t : s_coreHostThreads)
			s_coreHostThreadHandles.emplace_back(t.native_handle());
	}

	// stops the host core threads and frees all fibers
	void OSSchedulerEnd()
	{
		std::unique_lock guard(s_schedulerStartStopMutex);
		s_schedulerRunning.store(false);
		// wake cores that sleep waiting for work so they notice the stop
		for (auto& count : s_runQueueSize)
			count.increment();
		for (auto& t : s_coreHostThreads)
			t.join();
		s_coreHostThreads.clear();
		s_coreHostThreadHandles.clear();
#if BOOST_OS_LINUX
		{
			std::lock_guard idGuard(s_coreHostThreadIdsMutex);
			s_coreHostThreadIds.clear();
		}
#endif
		for (Fiber*& f : s_coreIdleFiber)
		{
			delete f;
			f = nullptr;
		}
		for (auto& entry : s_guestFibers)
			delete entry.second;
		s_guestFibers.clear();
	}

	bool OSIsSchedulerActive()
	{
		return s_schedulerRunning;
	}

	/* default threads */

	constexpr uint32 kDefaultThreadStackSize = 1024 * 1024;

	SysAllocator<OSThread_t, PPC_CORE_COUNT> s_coreDefaultThread;
	SysAllocator<uint8, PPC_CORE_COUNT * kDefaultThreadStackSize> s_coreDefaultThreadStack;

	OSThread_t* OSGetDefaultThread(sint32 coreIndex)
	{
		if (coreIndex < 0 || coreIndex >= PPC_CORE_COUNT)
			return nullptr;
		return s_coreDefaultThread.GetPtr() + coreIndex;
	}

	void* OSGetDefaultThreadStack(sint32 coreIndex, uint32& size)
	{
		if (coreIndex < 0 || coreIndex >= PPC_CORE_COUNT)
			return nullptr;
		size = kDefaultThreadStackSize;
		return s_coreDefaultThreadStack.GetPtr() + coreIndex * kDefaultThreadStackSize;
	}

	void OSInitThreadQueueEx(OSThreadQueue* threadQueue, void* userData)
	{
		threadQueue->head = nullptr;
		threadQueue->tail = nullptr;
		threadQueue->userData = userData;
		threadQueue->ukn0C = 0;
	}

	void OSInitThreadQueue(OSThreadQueue* threadQueue)
	{
		OSInitThreadQueueEx(threadQueue, nullptr);
	}

	/* deallocator threads */

	// A finished thread's deallocator must not run on the thread itself (it frees its stack), so it
	// is handed to a high priority I/O thread on the same core.
	struct ReaperThread
	{
		struct PendingRelease
		{
			OSThread_t* thread = nullptr;
			MEMPTR<void> stackEnd;
			MEMPTR<void> deallocator;
		};

		SysAllocator<OSThread_t> thread;
		SysAllocator<uint8, 16 * 1024> stack;
		SysAllocator<char, 64> name;
		SysAllocator<OSSemaphore> pendingCount;
		ConcurrentQueue<PendingRelease> pending;
	};

	ReaperThread s_reaper[PPC_CORE_COUNT];

	void ReaperThreadMain(PPCInterpreter_t* hCPU)
	{
		ReaperThread& reaper = s_reaper[OSGetCoreId()];
		while (OSWaitSemaphore(reaper.pendingCount.GetPtr()))
		{
			ReaperThread::PendingRelease job;
			reaper.pending.pop(job);
			PPCCoreCallback(job.deallocator, job.thread, job.stackEnd);
		}
		osLib_returnFromFunction(hCPU, 0);
	}

	void __OSQueueThreadDeallocation(OSThread_t* thread)
	{
		ReaperThread& reaper = s_reaper[OSGetCoreId()];
		ReaperThread::PendingRelease job;
		job.thread = thread;
		job.stackEnd = thread->stackEnd;
		job.deallocator = thread->deallocatorFunc;
		reaper.pending.push(job);
		// no reschedule: the exiting thread has to be switched out before its deallocator runs
		OSSignalSemaphoreInternal(reaper.pendingCount.GetPtr(), false);
	}

	void StartReaperThreads()
	{
		for (sint32 core = 0; core < PPC_CORE_COUNT; core++)
		{
			ReaperThread& reaper = s_reaper[core];
			OSInitSemaphore(reaper.pendingCount.GetPtr(), 0);
			snprintf(reaper.name.GetPtr(), reaper.name.GetByteSize(), "{SYS Thread Terminator Core %d}", core);
			uint32 stackSize = (uint32)reaper.stack.GetByteSize();
			MPTR entry = PPCInterpreter_makeCallableExportDepr(ReaperThreadMain);
			OSCreateThreadType(reaper.thread.GetPtr(), entry, 0, nullptr, reaper.stack.GetPtr() + stackSize, (sint32)stackSize, 0, 1u << core, OSThread_t::THREAD_TYPE::TYPE_IO);
			OSSetThreadName(reaper.thread.GetPtr(), reaper.name.GetPtr());
			OSResumeThread(reaper.thread.GetPtr());
		}
	}

	SysAllocator<char, 64> s_defaultThreadName[PPC_CORE_COUNT];

	void SetupDefaultThreads()
	{
		for (sint32 core = 0; core < PPC_CORE_COUNT; core++)
		{
			snprintf(s_defaultThreadName[core].GetPtr(), 64, "Default Core %d", core);
			uint32 stackSize;
			uint8* stack = (uint8*)OSGetDefaultThreadStack(core, stackSize);
			OSThread_t* thread = OSGetDefaultThread(core);
			// no entry point: the object exists for code that queries the default thread
			OSCreateThreadType(thread, MPTR_NULL, 0, nullptr, stack + stackSize, (sint32)stackSize, 16, 1u << core, OSThread_t::THREAD_TYPE::TYPE_APP);
			OSSetThreadName(thread, s_defaultThreadName[core].GetPtr());
		}
	}

	void MapThreadExports()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::CoreinitThread)
		EXPORT_FN(OSCreateThreadType); EXPORT_FN(OSCreateThread); EXPORT_FN(__OSCreateThreadType);
		EXPORT_FN(OSExitThread);

		EXPORT_FN(OSGetCurrentThread); EXPORT_FN(OSSetThreadSpecific); EXPORT_FN(OSGetThreadSpecific);
		EXPORT_FN(OSSetThreadName); EXPORT_FN(OSGetThreadName);

		EXPORT_FN(OSRunThread); EXPORT_FN(OSDetachThread); EXPORT_FN(OSJoinThread);

		EXPORT_FN(OSResumeThread); EXPORT_FN(OSContinueThread); EXPORT_FN(OSSuspendThread);
		EXPORT_FN(__OSSuspendThreadNolock); EXPORT_FN(OSSleepThread); EXPORT_FN(OSWakeupThread);

		EXPORT_FN(OSYieldThread); EXPORT_FN(OSSleepTicks);

		EXPORT_FN(OSSetThreadDeallocator); EXPORT_FN(OSSetThreadCleanupCallback); EXPORT_FN(OSSetThreadPriority);
		EXPORT_FN(OSGetThreadPriority); EXPORT_FN(OSSetThreadAffinity); EXPORT_FN(OSGetThreadAffinity);

		EXPORT_FN(OSIsThreadTerminated); EXPORT_FN(OSIsThreadSuspended); EXPORT_FN(OSCancelThread);
		EXPORT_FN(OSTestThreadCancel);

		EXPORT_FN(OSGetDefaultThread);

		EXPORT_FN(OSInitThreadQueue); EXPORT_FN(OSInitThreadQueueEx);
#undef EXPORT_FN
	}

	void InitializeThread()
	{
		OSInitThreadQueue(s_activeThreads.GetPtr());
		for (sint32 core = 0; core < Espresso::CORE_COUNT; core++)
			OSInitThreadQueue(s_runQueue.GetPtr() + core);
		for (auto& t : s_runningThread)
			t = nullptr;
		SetupDefaultThreads();
		StartReaperThreads();
	}
}
