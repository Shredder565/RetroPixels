// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "util/helpers/fspinlock.h"

// Deferred guest callbacks. Host side code (input, HID, keyboard) that has to call into the title
// queues the call here; a dedicated guest I/O thread runs the queued calls in order, so they
// happen on a guest thread instead of a host thread.

namespace
{
	struct PendingCallback
	{
		MPTR function;
		uint32 args[8]; // r3 to r10; all eight are always passed
	};

	FSpinlock s_pendingLock;
	std::deque<PendingCallback> s_pending;

	// guest memory, keep this definition order
	SysAllocator<OSThread_t> s_callbackThread;
	SysAllocator<uint8, 64 * 1024> s_callbackThreadStack;
	SysAllocator<coreinit::OSSemaphore> s_pendingCount;
	SysAllocator<char, 32> s_callbackThreadName;

	void RunNextPending()
	{
		s_pendingLock.lock();
		if (s_pending.empty())
		{
			s_pendingLock.unlock();
			cemuLog_log(LogType::Force, "Callback thread woke up without a queued callback");
			return;
		}
		PendingCallback cb = s_pending.front();
		s_pending.pop_front();
		s_pendingLock.unlock();
		PPCCoreCallback(cb.function, cb.args[0], cb.args[1], cb.args[2], cb.args[3], cb.args[4], cb.args[5], cb.args[6], cb.args[7]);
	}

	// guest entry point of the callback thread
	void CallbackThreadMain(PPCInterpreter_t* hCPU)
	{
		while (coreinit::OSWaitSemaphore(s_pendingCount.GetPtr()))
			RunNextPending();
	}
}

// queues a call; the caller must already hold the scheduler lock
void coreinitAsyncCallback_addWithLock(MPTR functionMPTR, uint32 numParameters, uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9, uint32 r10)
{
	cemu_assert_debug(numParameters <= 8);
	if (functionMPTR >= 0x10000000)
	{
		// callbacks normally point into title code; this usually means a corrupted pointer
		cemuLog_log(LogType::Force, fmt::format("Callback to unexpected address {:08x}, arguments {:08x} {:08x} {:08x} {:08x}", functionMPTR, r3, r4, r5, r6));
		cemuLog_waitForFlush();
	}
	s_pendingLock.lock();
	s_pending.push_back({ functionMPTR, { r3, r4, r5, r6, r7, r8, r9, r10 } });
	s_pendingLock.unlock();
	__OSLockScheduler();
	coreinit::OSSignalSemaphoreInternal(s_pendingCount.GetPtr(), false);
	__OSUnlockScheduler();
}

void coreinitAsyncCallback_add(MPTR functionMPTR, uint32 numParameters, uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9, uint32 r10)
{
	cemu_assert_debug(!__OSHasSchedulerLock());
	coreinitAsyncCallback_addWithLock(functionMPTR, numParameters, r3, r4, r5, r6, r7, r8, r9, r10);
}

void InitializeAsyncCallback()
{
	coreinit::OSInitSemaphore(s_pendingCount.GetPtr(), 0);
	uint8* stackTop = s_callbackThreadStack.GetPtr() + s_callbackThreadStack.GetByteSize();
	coreinit::OSCreateThreadType(s_callbackThread.GetPtr(), PPCInterpreter_makeCallableExportDepr(CallbackThreadMain), 0, nullptr, stackTop, (sint32)s_callbackThreadStack.GetByteSize(), 0, 7, OSThread_t::THREAD_TYPE::TYPE_IO);
	coreinit::OSResumeThread(s_callbackThread.GetPtr());
	strcpy(s_callbackThreadName.GetPtr(), "Callback Thread");
	coreinit::OSSetThreadName(s_callbackThread.GetPtr(), s_callbackThreadName.GetPtr());
}
