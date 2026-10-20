// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "util/helpers/fspinlock.h"

// Blocking primitives of coreinit: events, rendezvous, mutexes, condition variables, semaphores
// and the "fast" mutex/condition variants. All waiting goes through the thread queues of
// coreinit_ThreadQueue.cpp and happens under the scheduler lock.

namespace coreinit
{
	namespace
	{
		constexpr uint32 kEventMagic = 0x65566E54;     // 'eVnT'
		constexpr uint32 kCondMagic = 0x634E6456;      // 'cNdV'
		constexpr uint32 kSemaphoreMagic = 0x73506852; // 'sPhR'
		constexpr uint32 kFastMutexMagic = 0x664D7458; // 'fMtX'
		constexpr uint32 kFastCondMagic = 0x664E6456;  // 'fNdV'
		// OSMutex stores its magic in host byte order
		constexpr uint32 kMutexMagicHost = ((uint32)'m' << 24) | ((uint32)'U' << 16) | ((uint32)'t' << 8) | (uint32)'X';

		bool IsSignaled(const OSEvent* event)
		{
			return event->state == OSEvent::EVENT_STATE::STATE_SIGNALED;
		}

		// an auto reset event is consumed by the waiter that sees it signalled
		void ConsumeSignal(OSEvent* event)
		{
			if (event->mode == OSEvent::EVENT_MODE::MODE_AUTO)
				event->state = OSEvent::EVENT_STATE::STATE_NOT_SIGNALED;
		}
	}

	/* OSEvent */

	void OSInitEventEx(OSEvent* event, OSEvent::EVENT_STATE initialState, OSEvent::EVENT_MODE mode, void* userData)
	{
		event->magic = kEventMagic;
		event->ukn08 = 0;
		event->mode = mode;
		event->state = initialState;
		event->userData = userData;
		OSInitThreadQueueEx(&event->threadQueue, event);
	}

	void OSInitEvent(OSEvent* event, OSEvent::EVENT_STATE initialState, OSEvent::EVENT_MODE mode)
	{
		OSInitEventEx(event, initialState, mode, nullptr);
	}

	void OSResetEvent(OSEvent* event)
	{
		__OSLockScheduler();
		if (IsSignaled(event))
			event->state = OSEvent::EVENT_STATE::STATE_NOT_SIGNALED;
		__OSUnlockScheduler();
	}

	void OSWaitEventInternal(OSEvent* event)
	{
		if (IsSignaled(event))
			ConsumeSignal(event);
		else
			event->threadQueue.queueAndWait(OSGetCurrentThread());
	}

	void OSWaitEvent(OSEvent* event)
	{
		__OSLockScheduler();
		OSWaitEventInternal(event);
		__OSUnlockScheduler();
	}

	struct EventTimeoutState
	{
		OSThread_t* waiter;
		OSThreadQueue* queue;
		bool timedOut;
	};

	void EventTimeoutFired(uint64 currentTick, void* context)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		EventTimeoutState* state = (EventTimeoutState*)context;
		if (state->waiter->state != OSThread_t::THREAD_STATE::STATE_WAITING)
			return; // already woken by a signal
		state->timedOut = true;
		state->queue->cancelWait(state->waiter);
	}

	bool OSWaitEventWithTimeout(OSEvent* event, uint64 timeout)
	{
		__OSLockScheduler();
		if (IsSignaled(event))
		{
			ConsumeSignal(event);
			__OSUnlockScheduler();
			return true;
		}
		if (timeout == 0)
		{
			__OSUnlockScheduler();
			return false;
		}
		// Some Unity titles (Qube Director's Cut) ignore the result and decide whether the wait timed
		// out by measuring the elapsed time, so the wait ends a little early (98%)
		if (timeout < 0x00FFFFFFFFFFFFFFull)
			timeout = timeout * 98 / 100;
		EventTimeoutState state{ OSGetCurrentThread(), &event->threadQueue, false };
		OSHostAlarm* alarm = OSHostAlarmCreate(OSGetTime() + EspressoTime::ConvertNsToTimerTicks(timeout), 0, EventTimeoutFired, &state);
		event->threadQueue.queueAndWait(state.waiter);
		OSHostAlarmDestroy(alarm);
		__OSUnlockScheduler();
		return !state.timedOut;
	}

	void OSSignalEventInternal(OSEvent* event)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		if (IsSignaled(event))
			return;
		if (event->mode == OSEvent::EVENT_MODE::MODE_MANUAL)
		{
			event->state = OSEvent::EVENT_STATE::STATE_SIGNALED;
			event->threadQueue.wakeupEntireWaitQueue(true);
		}
		else if (event->threadQueue.isEmpty())
			event->state = OSEvent::EVENT_STATE::STATE_SIGNALED; // kept until a thread waits
		else
			event->threadQueue.wakeupSingleThreadWaitQueue(true); // handed directly to one waiter
	}

	void OSSignalEvent(OSEvent* event)
	{
		__OSLockScheduler();
		OSSignalEventInternal(event);
		__OSUnlockScheduler();
	}

	// like OSSignalEvent, but an auto reset event releases every waiter
	void OSSignalEventAllInternal(OSEvent* event)
	{
		if (IsSignaled(event))
			return;
		if (event->mode == OSEvent::EVENT_MODE::MODE_MANUAL || event->threadQueue.isEmpty())
			event->state = OSEvent::EVENT_STATE::STATE_SIGNALED;
		if (!event->threadQueue.isEmpty())
			event->threadQueue.wakeupEntireWaitQueue(true);
	}

	void OSSignalEventAll(OSEvent* event)
	{
		__OSLockScheduler();
		OSSignalEventAllInternal(event);
		__OSUnlockScheduler();
	}

	/* OSRendezvous */

	// all rendezvous share one event; every arrival wakes all waiters so they recheck their own object
	SysAllocator<OSEvent> s_rendezvousArrival;

	void OSInitRendezvous(OSRendezvous* rendezvous)
	{
		__OSLockScheduler();
		rendezvous->userData = rendezvous;
		for (auto& hit : rendezvous->coreHit)
			hit = 0;
		__OSUnlockScheduler();
	}

	bool OSWaitRendezvous(OSRendezvous* rendezvous, uint32 coreMask)
	{
		__OSLockScheduler();
		rendezvous->coreHit[OSGetCoreId()] = 1;
		OSSignalEventAllInternal(s_rendezvousArrival.GetPtr());
		auto everyoneArrived = [&]() {
			for (uint32 core = 0; core < PPC_CORE_COUNT; core++)
			{
				if ((coreMask & (1u << core)) != 0 && rendezvous->coreHit[core] == 0)
					return false;
			}
			return true;
		};
		while (!everyoneArrived())
			OSWaitEventInternal(s_rendezvousArrival.GetPtr());
		__OSUnlockScheduler();
		return true;
	}

	/* OSMutex */

	void OSInitMutexEx(OSMutex* mutex, void* userData)
	{
		mutex->magic = kMutexMagicHost;
		mutex->userData = userData;
		mutex->ukn08 = 0;
		mutex->owner = nullptr;
		mutex->lockCount = 0;
		OSInitThreadQueueEx(&mutex->threadQueue, mutex);
	}

	void OSInitMutex(OSMutex* mutex)
	{
		OSInitMutexEx(mutex, nullptr);
	}

	namespace
	{
		// takes the mutex if it is free or already ours; scheduler lock held
		bool TryTakeMutex(OSMutex* mutex, OSThread_t* self)
		{
			if (mutex->owner.IsNull())
			{
				cemu_assert_debug(mutex->lockCount == 0);
				mutex->owner = self;
				mutex->lockCount = 1;
				self->mutexQueue.addMutex(mutex);
				return true;
			}
			if (mutex->owner == self)
			{
				mutex->lockCount = mutex->lockCount + 1;
				return true;
			}
			return false;
		}
	}

	void OSLockMutexInternal(OSMutex* mutex)
	{
		OSThread_t* self = OSGetCurrentThread();
		uint32 contendedWaits = 0;
		while (!TryTakeMutex(mutex, self))
		{
			if (contendedWaits == 0x800)
				cemuLog_log(LogType::Force, "OSLockMutex: mutex 0x{:08x} has been contended for a long time", MEMPTR<OSMutex>(mutex).GetMPTR());
			self->waitingForMutex = mutex;
			mutex->threadQueue.queueAndWait(self);
			self->waitingForMutex = nullptr;
			contendedWaits++;
		}
	}

	void OSLockMutex(OSMutex* mutex)
	{
		__OSLockScheduler();
		OSTestThreadCancelInternal();
		OSLockMutexInternal(mutex);
		__OSUnlockScheduler();
	}

	bool OSTryLockMutex(OSMutex* mutex)
	{
		OSThread_t* self = OSGetCurrentThread();
		__OSLockScheduler();
		OSTestThreadCancelInternal();
		bool taken = TryTakeMutex(mutex, self);
		__OSUnlockScheduler();
		return taken;
	}

	void OSUnlockMutexInternal(OSMutex* mutex)
	{
		OSThread_t* self = OSGetCurrentThread();
		if (mutex->lockCount == 0)
		{
			cemuLog_log(LogType::APIErrors, "OSUnlockMutex: mutex 0x{:08x} is not locked", MEMPTR<OSMutex>(mutex).GetMPTR());
			return;
		}
		if (mutex->owner != self)
		{
			cemuLog_log(LogType::APIErrors, "OSUnlockMutex: mutex 0x{:08x} is owned by thread 0x{:08x}, not by the caller 0x{:08x}", MEMPTR<OSMutex>(mutex).GetMPTR(), mutex->owner.GetMPTR(), MEMPTR<OSThread_t>(self).GetMPTR());
			return;
		}
		mutex->lockCount = mutex->lockCount - 1;
		if (mutex->lockCount != 0)
			return;
		self->mutexQueue.removeMutex(mutex);
		mutex->owner = nullptr;
		if (!mutex->threadQueue.isEmpty())
			mutex->threadQueue.wakeupSingleThreadWaitQueue(true, true);
	}

	void OSUnlockMutex(OSMutex* mutex)
	{
		__OSLockScheduler();
		OSUnlockMutexInternal(mutex);
		__OSUnlockScheduler();
	}

	/* OSCond */

	void OSInitCond(OSCond* cond)
	{
		cond->magic = kCondMagic;
		cond->userData = nullptr;
		cond->ukn08 = 0;
		OSInitThreadQueueEx(&cond->threadQueue, cond);
	}

	void OSInitCondEx(OSCond* cond, void* userData)
	{
		OSInitCond(cond);
		cond->userData = userData;
	}

	void OSSignalCond(OSCond* cond)
	{
		// wakes all waiters; each one re-acquires the mutex in turn
		OSWakeupThread(&cond->threadQueue);
	}

	void OSWaitCond(OSCond* cond, OSMutex* mutex)
	{
		__OSLockScheduler();
		OSThread_t* self = OSGetCurrentThread();
		cemu_assert_debug(mutex->owner == self);
		// release the mutex completely, whatever its recursion depth (used by Bayonetta 2)
		sint32 savedDepth = mutex->lockCount;
		mutex->lockCount = 0;
		self->mutexQueue.removeMutex(mutex);
		mutex->owner = nullptr;
		if (!mutex->threadQueue.isEmpty())
			mutex->threadQueue.wakeupEntireWaitQueue(false);
		cond->threadQueue.queueAndWait(self);
		OSLockMutexInternal(mutex);
		mutex->lockCount = savedDepth;
		__OSUnlockScheduler();
	}

	/* OSSemaphore */

	void OSInitSemaphoreEx(OSSemaphore* semaphore, sint32 initialCount, void* userData)
	{
		__OSLockScheduler();
		semaphore->magic = kSemaphoreMagic;
		semaphore->userData = userData;
		semaphore->ukn08 = 0;
		semaphore->count = initialCount;
		OSInitThreadQueueEx(&semaphore->threadQueue, semaphore);
		__OSUnlockScheduler();
	}

	void OSInitSemaphore(OSSemaphore* semaphore, sint32 initialCount)
	{
		OSInitSemaphoreEx(semaphore, initialCount, nullptr);
	}

	// returns the count before the decrement
	sint32 OSWaitSemaphoreInternal(OSSemaphore* semaphore)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		while (semaphore->count <= 0)
			semaphore->threadQueue.queueAndWait(OSGetCurrentThread());
		sint32 previous = semaphore->count;
		semaphore->count = previous - 1;
		return previous;
	}

	sint32 OSWaitSemaphore(OSSemaphore* semaphore)
	{
		__OSLockScheduler();
		sint32 previous = OSWaitSemaphoreInternal(semaphore);
		__OSUnlockScheduler();
		return previous;
	}

	sint32 OSTryWaitSemaphore(OSSemaphore* semaphore)
	{
		__OSLockScheduler();
		sint32 previous = semaphore->count;
		if (previous > 0)
			semaphore->count = previous - 1;
		__OSUnlockScheduler();
		return previous;
	}

	sint32 OSSignalSemaphoreInternal(OSSemaphore* semaphore, bool reschedule)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		sint32 previous = semaphore->count;
		semaphore->count = previous + 1;
		// all waiters recheck the count, the first to run takes it
		semaphore->threadQueue.wakeupEntireWaitQueue(reschedule);
		return previous;
	}

	sint32 OSSignalSemaphore(OSSemaphore* semaphore)
	{
		__OSLockScheduler();
		sint32 previous = OSSignalSemaphoreInternal(semaphore, true);
		__OSUnlockScheduler();
		return previous;
	}

	sint32 OSGetSemaphoreCount(OSSemaphore* semaphore)
	{
		__OSLockScheduler();
		sint32 count = semaphore->count;
		__OSUnlockScheduler();
		return count;
	}

	/* OSFastMutex */

	// Fast mutexes are taken without the scheduler lock. One host spinlock serialises the
	// owner/count updates of all fast mutexes; the scheduler lock is only taken to block or wake.
	FSpinlock s_fastMutexGuard;

	void OSFastMutex_Init(OSFastMutex* fastMutex, void* userData)
	{
		fastMutex->magic = kFastMutexMagic;
		fastMutex->userData = userData;
		fastMutex->contendedState = 0;
		fastMutex->owner = nullptr;
		fastMutex->lockCount = 0;
		fastMutex->threadQueueSmall.head = nullptr;
		fastMutex->threadQueueSmall.tail = nullptr;
		fastMutex->ownedLink.next = nullptr;
		fastMutex->ownedLink.prev = nullptr;
		fastMutex->contendedLink.next = nullptr;
		fastMutex->contendedLink.prev = nullptr;
	}

	namespace
	{
		// with s_fastMutexGuard held
		bool TryTakeFastMutex(OSFastMutex* fastMutex, OSThread_t* self)
		{
			if (fastMutex->owner.atomic_compare_exchange(nullptr, self))
			{
				cemu_assert_debug(fastMutex->lockCount == 0);
				fastMutex->lockCount = 1;
				return true;
			}
			if (fastMutex->owner == self)
			{
				fastMutex->lockCount = fastMutex->lockCount + 1;
				return true;
			}
			return false;
		}
	}

	void OSFastMutex_LockInternal(OSFastMutex* fastMutex)
	{
		cemu_assert_debug(!__OSHasSchedulerLock());
		OSThread_t* self = OSGetCurrentThread();
		s_fastMutexGuard.lock();
		while (!TryTakeFastMutex(fastMutex, self))
		{
			// enqueue before dropping the guard so an unlock in between cannot miss us
			self->waitingForFastMutex = fastMutex;
			__OSLockScheduler();
			fastMutex->threadQueueSmall.queueOnly(self);
			s_fastMutexGuard.unlock();
			PPCCore_switchToSchedulerWithLock();
			self->waitingForFastMutex = nullptr;
			__OSUnlockScheduler();
			s_fastMutexGuard.lock();
		}
		s_fastMutexGuard.unlock();
	}

	void OSFastMutex_Lock(OSFastMutex* fastMutex)
	{
		OSFastMutex_LockInternal(fastMutex);
	}

	bool OSFastMutex_TryLock(OSFastMutex* fastMutex)
	{
		OSThread_t* self = OSGetCurrentThread();
		s_fastMutexGuard.lock();
		bool taken = TryTakeFastMutex(fastMutex, self);
		s_fastMutexGuard.unlock();
		return taken;
	}

	void OSFastMutex_Unlock(OSFastMutex* fastMutex)
	{
		cemu_assert_debug(!__OSHasSchedulerLock());
		OSThread_t* self = OSGetCurrentThread();
		s_fastMutexGuard.lock();
		// unlocking a fast mutex the caller does not own is ignored (Paper Mario Color Splash does it)
		if (fastMutex->owner == self)
		{
			cemu_assert_debug(fastMutex->lockCount > 0);
			fastMutex->lockCount = fastMutex->lockCount - 1;
			if (fastMutex->lockCount == 0)
			{
				bool released = fastMutex->owner.atomic_compare_exchange(self, nullptr);
				cemu_assert_debug(released);
				(void)released;
				if (!fastMutex->threadQueueSmall.isEmpty())
				{
					__OSLockScheduler();
					fastMutex->threadQueueSmall.wakeupSingleThreadWaitQueue(false);
					__OSUnlockScheduler();
				}
			}
		}
		s_fastMutexGuard.unlock();
	}

	/* OSFastCond */

	void OSFastCond_Init(OSFastCond* fastCond, void* userData)
	{
		fastCond->magic = kFastCondMagic;
		fastCond->userData = userData;
		fastCond->ukn08 = 0;
		OSInitThreadQueueEx(&fastCond->threadQueue, fastCond);
	}

	void OSFastCond_Wait(OSFastCond* fastCond, OSFastMutex* fastMutex)
	{
		__OSLockScheduler();
		OSThread_t* self = OSGetCurrentThread();
		cemu_assert_debug(fastMutex->owner == self);
		uint32 savedDepth = fastMutex->lockCount;
		fastMutex->lockCount = 0;
		fastMutex->owner = nullptr;
		if (!fastMutex->threadQueueSmall.isEmpty())
			fastMutex->threadQueueSmall.wakeupEntireWaitQueue(false);
		fastCond->threadQueue.queueAndWait(self);
		__OSUnlockScheduler();
		OSFastMutex_LockInternal(fastMutex);
		fastMutex->lockCount = savedDepth;
	}

	void OSFastCond_Signal(OSFastCond* fastCond)
	{
		OSWakeupThread(&fastCond->threadQueue);
	}

	void InitializeConcurrency()
	{
		OSInitEvent(s_rendezvousArrival.GetPtr(), OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, OSEvent::EVENT_MODE::MODE_AUTO);

		cafeExportRegister("coreinit", OSInitEvent, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSInitEventEx, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSResetEvent, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitEvent, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitEventWithTimeout, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSSignalEvent, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSSignalEventAll, LogType::CoreinitThreadSync);

		cafeExportRegister("coreinit", OSInitRendezvous, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitRendezvous, LogType::CoreinitThreadSync);

		cafeExportRegister("coreinit", OSInitMutex, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSInitMutexEx, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSLockMutex, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSTryLockMutex, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSUnlockMutex, LogType::CoreinitThreadSync);

		cafeExportRegister("coreinit", OSInitCond, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSInitCondEx, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSSignalCond, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitCond, LogType::CoreinitThreadSync);

		cafeExportRegister("coreinit", OSInitSemaphore, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSInitSemaphoreEx, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSWaitSemaphore, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSTryWaitSemaphore, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSSignalSemaphore, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSGetSemaphoreCount, LogType::CoreinitThreadSync);

		cafeExportRegister("coreinit", OSFastMutex_Init, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastMutex_Lock, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastMutex_TryLock, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastMutex_Unlock, LogType::CoreinitThreadSync);

		cafeExportRegister("coreinit", OSFastCond_Init, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastCond_Wait, LogType::CoreinitThreadSync);
		cafeExportRegister("coreinit", OSFastCond_Signal, LogType::CoreinitThreadSync);
	}
}
