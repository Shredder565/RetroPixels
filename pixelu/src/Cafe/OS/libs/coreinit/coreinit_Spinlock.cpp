// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Spinlock.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"

namespace coreinit
{
	namespace
	{
		// set in stateFlags (host byte order) while the thread is boosted by a spinlock
		constexpr uint32 kStateFlagSpinlockBoost = 0x20000;

		// a spinlock holder runs at the highest priority so it can't be starved while others spin
		void RaiseHolderPriority(OSThread_t* thread)
		{
			__OSLockScheduler();
			thread->stateFlags |= kStateFlagSpinlockBoost;
			thread->context.boostCount = thread->context.boostCount + 1;
			__OSUpdateThreadEffectivePriority(thread);
			__OSUnlockScheduler();
		}

		void DropHolderPriority(OSThread_t* thread)
		{
			__OSLockScheduler();
			cemu_assert_debug(thread->context.boostCount != 0);
			thread->context.boostCount = thread->context.boostCount - 1;
			if (thread->context.boostCount == 0)
			{
				thread->stateFlags &= ~kStateFlagSpinlockBoost;
				__OSUpdateThreadEffectivePriority(thread);
			}
			__OSUnlockScheduler();
		}

		bool TryClaim(OSSpinLock* spinlock, OSThread_t* self)
		{
			return spinlock->ownerThread.atomic_compare_exchange(nullptr, self);
		}

		// recursion: the owner only bumps the count
		bool AlreadyOwned(OSSpinLock* spinlock, OSThread_t* self)
		{
			if (spinlock->ownerThread != self)
				return false;
			spinlock->count = spinlock->count + 1;
			return true;
		}

		void SpinYielding(OSSpinLock* spinlock, OSThread_t* self)
		{
			while (!TryClaim(spinlock, self))
				OSYieldThread();
		}

		// timeout in nanoseconds; false once it has passed without getting the lock
		bool SpinYieldingWithTimeout(OSSpinLock* spinlock, OSThread_t* self, uint64 timeout)
		{
			cemu_assert_debug((sint64)timeout >= 0);
			uint64 deadline = OSGetSystemTime() + EspressoTime::ConvertNsToTimerTicks(timeout);
			while (!TryClaim(spinlock, self))
			{
				OSYieldThread();
				if (OSGetSystemTime() >= deadline)
					return false;
			}
			return true;
		}

		bool ReleaseCommon(OSSpinLock* spinlock, OSThread_t* self, bool restoreInterrupts)
		{
			cemu_assert_debug(spinlock->ownerThread == self);
			if (spinlock->count != 0)
			{
				spinlock->count = spinlock->count - 1;
				return true;
			}
			if (restoreInterrupts)
			{
				OSRestoreInterrupts(spinlock->interruptMask);
				spinlock->interruptMask = 1;
			}
			while (!spinlock->ownerThread.atomic_compare_exchange(self, nullptr))
				;
			DropHolderPriority(self);
			return true;
		}

		void FinishUninterruptibleAcquire(OSSpinLock* spinlock, OSThread_t* self)
		{
			RaiseHolderPriority(self);
			spinlock->interruptMask = OSDisableInterrupts();
		}
	}

	void OSInitSpinLock(OSSpinLock* spinlock)
	{
		spinlock->ownerThread = nullptr;
		spinlock->userData = spinlock;
		spinlock->count = 0;
		spinlock->interruptMask = 1;
	}

	bool OSAcquireSpinLock(OSSpinLock* spinlock)
	{
		OSThread_t* self = OSGetCurrentThread();
		if (AlreadyOwned(spinlock, self))
			return true;
		SpinYielding(spinlock, self);
		RaiseHolderPriority(self);
		return true;
	}

	bool OSTryAcquireSpinLock(OSSpinLock* spinlock)
	{
		OSThread_t* self = OSGetCurrentThread();
		if (AlreadyOwned(spinlock, self))
			return true;
		if (!TryClaim(spinlock, self))
			return false;
		RaiseHolderPriority(self);
		return true;
	}

	// used by Call of Duty: Ghosts
	bool OSTryAcquireSpinLockWithTimeout(OSSpinLock* spinlock, uint64 timeout)
	{
		OSThread_t* self = OSGetCurrentThread();
		if (AlreadyOwned(spinlock, self))
			return true;
		if (!SpinYieldingWithTimeout(spinlock, self, timeout))
			return false;
		RaiseHolderPriority(self);
		return true;
	}

	bool OSReleaseSpinLock(OSSpinLock* spinlock)
	{
		return ReleaseCommon(spinlock, OSGetCurrentThread(), false);
	}

	bool OSUninterruptibleSpinLock_Acquire(OSSpinLock* spinlock)
	{
		OSThread_t* self = OSGetCurrentThread();
		cemu_assert_debug(self);
		if (AlreadyOwned(spinlock, self))
			return true;
		if (__CemuIsMulticoreMode())
		{
			// the holder runs on another host thread, plain spinning is fine
			while (!TryClaim(spinlock, self))
				_mm_pause();
		}
		else
		{
			// with one host thread the holder can only make progress if we yield, even though the
			// caller asked not to be interrupted
			if (!OSIsInterruptEnabled())
				cemuLog_logOnce(LogType::APIErrors, "OSUninterruptibleSpinLock_Acquire: lock is busy while interrupts are already disabled; yielding anyway (nested OSDisableInterrupts or uninterruptible spinlocks)");
			SpinYielding(spinlock, self);
		}
		FinishUninterruptibleAcquire(spinlock, self);
		return true;
	}

	bool OSUninterruptibleSpinLock_TryAcquire(OSSpinLock* spinlock)
	{
		OSThread_t* self = OSGetCurrentThread();
		if (AlreadyOwned(spinlock, self))
			return true;
		if (!TryClaim(spinlock, self))
			return false;
		FinishUninterruptibleAcquire(spinlock, self);
		return true;
	}

	bool OSUninterruptibleSpinLock_TryAcquireWithTimeout(OSSpinLock* spinlock, uint64 timeout)
	{
		OSThread_t* self = OSGetCurrentThread();
		if (AlreadyOwned(spinlock, self))
			return true;
		if (!SpinYieldingWithTimeout(spinlock, self, timeout))
			return false;
		FinishUninterruptibleAcquire(spinlock, self);
		return true;
	}

	bool OSUninterruptibleSpinLock_Release(OSSpinLock* spinlock)
	{
		return ReleaseCommon(spinlock, OSGetCurrentThread(), true);
	}

	void InitializeSpinlock()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::Placeholder)
		EXPORT_FN(OSInitSpinLock); EXPORT_FN(OSAcquireSpinLock); EXPORT_FN(OSTryAcquireSpinLock);
		EXPORT_FN(OSTryAcquireSpinLockWithTimeout); EXPORT_FN(OSReleaseSpinLock);

		EXPORT_FN(OSUninterruptibleSpinLock_Acquire); EXPORT_FN(OSUninterruptibleSpinLock_TryAcquire);
		EXPORT_FN(OSUninterruptibleSpinLock_TryAcquireWithTimeout); EXPORT_FN(OSUninterruptibleSpinLock_Release);
#undef EXPORT_FN
	}
}
