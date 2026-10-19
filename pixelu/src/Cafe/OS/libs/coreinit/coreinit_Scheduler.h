// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// The scheduler lock serialises all thread, run queue and wait queue state. It is held across a
// fiber switch, so it is taken on one guest thread and released on another on the same host thread.
void __OSLockScheduler(void* obj = nullptr);
bool __OSHasSchedulerLock();
bool __OSTryLockScheduler(void* obj = nullptr);
void __OSUnlockScheduler(void* obj = nullptr);

namespace coreinit
{
	// per core interrupt mask; with interrupts disabled the running thread is not preempted
	uint32 OSIsInterruptEnabled();
	uint32 OSDisableInterrupts();
	uint32 OSRestoreInterrupts(uint32 interruptMask);
	uint32 OSEnableInterrupts();

	void InitializeSchedulerLock();
}
