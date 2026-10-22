// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_Scheduler.h"

#if BOOST_OS_WINDOWS
#include <synchapi.h>
#else
#include <pthread.h>
#endif

namespace
{
	// recursive host mutex owned by the host thread (not the fiber) that took it
	class SchedulerMutex
	{
	public:
		void Init()
		{
#if BOOST_OS_WINDOWS
			InitializeCriticalSection(&m_cs);
#else
			pthread_mutexattr_t attr;
			pthread_mutexattr_init(&attr);
			pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
			pthread_mutex_init(&m_mutex, &attr);
			pthread_mutexattr_destroy(&attr);
#endif
		}

		void Lock()
		{
#if BOOST_OS_WINDOWS
			EnterCriticalSection(&m_cs);
#else
			pthread_mutex_lock(&m_mutex);
#endif
		}

		bool TryLock()
		{
#if BOOST_OS_WINDOWS
			return TryEnterCriticalSection(&m_cs) != 0;
#else
			return pthread_mutex_trylock(&m_mutex) == 0;
#endif
		}

		void Unlock()
		{
#if BOOST_OS_WINDOWS
			LeaveCriticalSection(&m_cs);
#else
			pthread_mutex_unlock(&m_mutex);
#endif
		}

	private:
#if BOOST_OS_WINDOWS
		CRITICAL_SECTION m_cs;
#else
		pthread_mutex_t m_mutex;
#endif
	};

	SchedulerMutex s_schedulerMutex;
	// depth on this host thread; the scheduler lock is never meant to be taken recursively
	thread_local sint32 t_schedulerLockDepth = 0;

	// while interrupts are off the running thread gets this many extra cycles so its timeslice
	// effectively never runs out
	constexpr sint32 kInterruptsOffCycleBonus = 0x40000000;
}

void __OSLockScheduler(void* obj)
{
	s_schedulerMutex.Lock();
	t_schedulerLockDepth++;
	cemu_assert_debug(t_schedulerLockDepth == 1);
}

bool __OSHasSchedulerLock()
{
	return t_schedulerLockDepth > 0;
}

bool __OSTryLockScheduler(void* obj)
{
	if (!s_schedulerMutex.TryLock())
		return false;
	t_schedulerLockDepth++;
	return true;
}

void __OSUnlockScheduler(void* obj)
{
	t_schedulerLockDepth--;
	cemu_assert_debug(t_schedulerLockDepth >= 0);
	s_schedulerMutex.Unlock();
}

namespace coreinit
{
	uint32 OSIsInterruptEnabled()
	{
		PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
		return cpu ? cpu->coreInterruptMask : 0;
	}

	uint32 OSDisableInterrupts()
	{
		PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
		if (!cpu)
			return 0;
		uint32 previous = cpu->coreInterruptMask;
		if (previous != 0)
		{
			if (cpu->remainingCycles >= kInterruptsOffCycleBonus)
				cemuLog_log(LogType::Force, "OSDisableInterrupts: interrupts look disabled already (remaining cycles {:08x}, LR {:08x})", cpu->remainingCycles, cpu->spr.LR);
			cpu->remainingCycles += kInterruptsOffCycleBonus;
		}
		cpu->coreInterruptMask = 0;
		return previous;
	}

	uint32 OSRestoreInterrupts(uint32 interruptMask)
	{
		PPCInterpreter_t* cpu = PPCInterpreter_getCurrentInstance();
		if (!cpu)
			return 0;
		uint32 previous = cpu->coreInterruptMask;
		if (previous == 0 && interruptMask != 0)
			cpu->remainingCycles -= kInterruptsOffCycleBonus;
		cpu->coreInterruptMask = interruptMask;
		return previous;
	}

	uint32 OSEnableInterrupts()
	{
		return OSRestoreInterrupts(1);
	}

	void InitializeSchedulerLock()
	{
		s_schedulerMutex.Init();

		cafeExportRegister("coreinit", __OSLockScheduler, LogType::Placeholder);
		cafeExportRegister("coreinit", __OSUnlockScheduler, LogType::Placeholder);

		cafeExportRegister("coreinit", OSDisableInterrupts, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSEnableInterrupts, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSRestoreInterrupts, LogType::CoreinitThread);
	}
}
