// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

namespace coreinit
{
	// recursive spinlock in guest memory. While held, the owner runs at boosted priority; the
	// uninterruptible variants also disable interrupts and keep the previous mask here
	struct OSSpinLock
	{
		/* +0x00 */ MEMPTR<struct OSThread_t> ownerThread;
		/* +0x04 */ MEMPTR<void> userData;
		/* +0x08 */ uint32be count;         // extra acquisitions by the owner
		/* +0x0C */ uint32be interruptMask; // saved by the uninterruptible acquire
	};
	static_assert(sizeof(OSSpinLock) == 0x10);

	void InitializeSpinlock();

	void OSInitSpinLock(OSSpinLock* spinlock);

	bool OSAcquireSpinLock(OSSpinLock* spinlock);
	bool OSTryAcquireSpinLock(OSSpinLock* spinlock);
	bool OSTryAcquireSpinLockWithTimeout(OSSpinLock* spinlock, uint64 timeout);
	bool OSReleaseSpinLock(OSSpinLock* spinlock);

	bool OSUninterruptibleSpinLock_Acquire(OSSpinLock* spinlock);
	bool OSUninterruptibleSpinLock_TryAcquire(OSSpinLock* spinlock);
	bool OSUninterruptibleSpinLock_TryAcquireWithTimeout(OSSpinLock* spinlock, uint64 timeout);
	bool OSUninterruptibleSpinLock_Release(OSSpinLock* spinlock);
}
