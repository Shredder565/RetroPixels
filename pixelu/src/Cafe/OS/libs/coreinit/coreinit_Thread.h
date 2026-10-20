// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Espresso/Const.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"

// Guest thread objects, the queues threads wait on and the synchronisation primitives built on
// them. All of these live in guest memory; their layouts match the console.

struct OSThread_t;

// FPSCR is saved as the low half of a 64 bit slot
struct OSContextRegFPSCR_t
{
	/* +0x00 */ uint32be padding;
	/* +0x04 */ uint32be fpscr;
};

// saved register state of a thread. Fields typed uint32 hold big endian values written with
// _swapEndianU32 (gpr, lr) or host values (cr, ctr, xer, srr0, srr1, gqr), as noted per field
struct OSContext_t
{
	static constexpr uint32 OS_CONTEXT_MAGIC_0 = 0x4f53436f; // 'OSCo'
	static constexpr uint32 OS_CONTEXT_MAGIC_1 = 0x6e747874; // 'ntxt'

	/* +0x000 */ betype<uint32> magic0;
	/* +0x004 */ betype<uint32> magic1;
	/* +0x008 */ uint32 gpr[32];          // big endian
	/* +0x088 */ uint32 cr;               // host order
	/* +0x08C */ uint32 lr;               // big endian
	/* +0x090 */ uint32 ctr;              // host order
	/* +0x094 */ uint32 xer;              // host order
	/* +0x098 */ uint32 srr0;             // host order, resume address
	/* +0x09C */ uint32 srr1;             // host order
	/* +0x0A0 */ uint32 dsi_dsisr;
	/* +0x0A4 */ uint32 dsi_dar;
	/* +0x0A8 */ uint32 ukn0A8;
	/* +0x0AC */ uint32 ukn0AC;
	/* +0x0B0 */ OSContextRegFPSCR_t fpscr;
	/* +0x0B8 */ uint64be fp_ps0[32];
	/* +0x1B8 */ uint16be boostCount;     // non-zero while a spinlock holder is boosted
	/* +0x1BA */ uint16 state;
	/* +0x1BC */ uint32 gqr[8];           // host order
	/* +0x1DC */ uint32be upir;           // core the thread last ran on
	/* +0x1E0 */ uint64be fp_ps1[32];
	/* +0x2E0 */ uint64be coretime[3];
	/* +0x2F8 */ uint64be starttime;
	/* +0x300 */ sint32be ghs_errno;      // errno of the GHS runtime (__gh_errno_ptr)
	/* +0x304 */ uint32be affinity;       // core mask, bits 0-2
	/* +0x308 */ uint32be upmc1;
	/* +0x30C */ uint32be upmc2;
	/* +0x310 */ uint32be upmc3;
	/* +0x314 */ uint32be upmc4;
	/* +0x318 */ uint32be ummcr0;
	/* +0x31C */ uint32be ummcr1;

	// "OSContxt" in the two magic words marks an initialised context
	void SetContextMagic() { magic0 = OS_CONTEXT_MAGIC_0; magic1 = OS_CONTEXT_MAGIC_1; }
	bool checkMagic() { return (uint32)magic0 == (uint32)OS_CONTEXT_MAGIC_0 && (uint32)magic1 == (uint32)OS_CONTEXT_MAGIC_1; }

	bool hasCoreAffinitySet(uint32 coreIndex) const { return (((uint32)affinity >> coreIndex) & 1) != 0; }

	void setAffinity(uint32 mask)
	{
		affinity = mask & 7;
	}

	uint32 getAffinity() const
	{
		return affinity;
	}
};
static_assert(offsetof(OSContext_t, fpscr) == 0xB0);
static_assert(offsetof(OSContext_t, boostCount) == 0x1B8);
static_assert(offsetof(OSContext_t, upir) == 0x1DC);
static_assert(offsetof(OSContext_t, ghs_errno) == 0x300);
static_assert(offsetof(OSContext_t, affinity) == 0x304);
static_assert(sizeof(OSContext_t) == 0x320);

// per thread C runtime data (C++ exception handling state)
typedef struct
{
	/* +0x000 */ uint32 ukn000[0x68 / 4];
	/* +0x068 */ MEMPTR<void> eh_globals;
	/* +0x06C */ uint32 eh_mem_manage[9];
	/* +0x090 */ MPTR eh_store_globals[6];
	/* +0x0A8 */ MPTR eh_store_globals_tdeh[76];
} crt_t;
static_assert(sizeof(crt_t) == 0x1D8);

#pragma pack(1)

namespace coreinit
{
	/* thread queues */

	// next/prev pointers embedded in OSThread_t; a thread has several, one per queue kind
	struct OSThreadLink
	{
		MEMPTR<struct OSThread_t> next;
		MEMPTR<struct OSThread_t> prev;
	};
	static_assert(sizeof(OSThreadLink) == 8);

	// doubly linked list of threads. Which embedded link is used is given per call
	struct OSThreadQueueInternal
	{
		MEMPTR<OSThread_t> head;
		MEMPTR<OSThread_t> tail;

		bool isEmpty() const
		{
			return head.IsNull();
		}

		void addThread(OSThread_t* thread, OSThreadLink* threadLink);           // append at the tail
		void addThreadByPriority(OSThread_t* thread, OSThreadLink* threadLink); // after all threads of equal or higher priority
		void removeThread(OSThread_t* thread, OSThreadLink* threadLink);

		// these use thread->waitQueueLink. queueAndWait blocks the thread (state WAITING) and
		// switches away; queueOnly enqueues and marks it WAITING without switching
		void queueAndWait(OSThread_t* thread);
		void queueOnly(OSThread_t* thread);

		// takes a waiting thread out of the queue and makes it ready
		void cancelWait(OSThread_t* thread);
		// make all (or the first) waiting threads ready. With reschedule set the caller yields if a
		// woken thread should run on this core instead
		void wakeupEntireWaitQueue(bool reschedule, bool sharedPriorityAndAffinityWorkaround = false);
		void wakeupSingleThreadWaitQueue(bool reschedule, bool sharedPriorityAndAffinityWorkaround = false);
	};
	static_assert(sizeof(OSThreadQueueInternal) == 0x8);

	// two word variant used inside OSFastMutex
	struct OSThreadQueueSmall : public OSThreadQueueInternal
	{
	};
	static_assert(sizeof(OSThreadQueueSmall) == 8);

	struct OSThreadQueue : public OSThreadQueueInternal
	{
		MEMPTR<void> userData;
		uint32be ukn0C;
	};
	static_assert(offsetof(OSThreadQueue, userData) == 0x8);
	static_assert(offsetof(OSThreadQueue, ukn0C) == 0xC);
	static_assert(sizeof(OSThreadQueue) == 0x10);

	/* OSMutex */

	struct OSMutex
	{
		/* +0x00 */ uint32 magic;              // 'mUtX', stored in host byte order
		/* +0x04 */ MEMPTR<void> userData;
		/* +0x08 */ uint32be ukn08;
		/* +0x0C */ OSThreadQueue threadQueue;  // threads waiting for the mutex
		/* +0x1C */ MEMPTR<OSThread_t> owner;
		/* +0x20 */ sint32be lockCount;         // recursion depth
		/* +0x24 */ MEMPTR<OSMutex> next;       // links in the owner's mutexQueue
		/* +0x28 */ MEMPTR<OSMutex> prev;
	};
	static_assert(sizeof(OSMutex) == 0x2C);

	// list of the mutexes a thread holds
	struct OSMutexQueue
	{
		MEMPTR<OSMutex> head;
		MEMPTR<OSMutex> tail;
		MEMPTR<void> ukn08;
		uint32be ukn0C;

		bool isEmpty() const
		{
			return head.IsNull();
		}

		void addMutex(OSMutex* mutex);
		void removeMutex(OSMutex* mutex);

		OSMutex* getFirst()
		{
			return head.GetPtr();
		}
	};
	static_assert(sizeof(OSMutexQueue) == 0x10);

	/* OSFastMutex */

	struct OSFastMutexLink
	{
		/* +0x00 */ MEMPTR<struct OSMutex> next;
		/* +0x04 */ MEMPTR<struct OSMutex> prev;
	};

	struct OSFastMutex
	{
		/* +0x00 */ uint32be magic;
		/* +0x04 */ MEMPTR<void> userData;
		/* +0x08 */ uint32be contendedState;
		/* +0x0C */ OSThreadQueueSmall threadQueueSmall;
		/* +0x14 */ OSFastMutexLink ownedLink;
		/* +0x1C */ MEMPTR<OSThread_t> owner;
		/* +0x20 */ uint32be lockCount;
		/* +0x24 */ OSFastMutexLink contendedLink;
	};
	static_assert(offsetof(OSFastMutex, owner) == 0x1C);
	static_assert(sizeof(OSFastMutex) == 0x2C);

	/* OSEvent */

	struct OSEvent
	{
		enum class EVENT_MODE : uint32
		{
			MODE_MANUAL = 0, // stays signalled until reset, wakes all waiters
			MODE_AUTO = 1,   // a signal releases one waiter
		};

		enum class EVENT_STATE : uint32
		{
			STATE_NOT_SIGNALED = 0,
			STATE_SIGNALED = 1
		};

		/* +0x00 */ uint32be magic; // 'eVnT'
		/* +0x04 */ MEMPTR<void> userData;
		/* +0x08 */ uint32be ukn08;
		/* +0x0C */ betype<EVENT_STATE> state;
		/* +0x10 */ OSThreadQueue threadQueue;
		/* +0x20 */ betype<EVENT_MODE> mode;
	};
	static_assert(sizeof(OSEvent) == 0x24);

	/* OSRendezvous */

	struct OSRendezvous
	{
		/* +0x00 */ uint32be coreHit[3];
		/* +0x0C */ MEMPTR<void> userData;
	};
	static_assert(sizeof(OSRendezvous) == 0x10);

	/* OSCond */

	struct OSCond
	{
		uint32be magic;
		MEMPTR<void> userData;
		uint32be ukn08;
		OSThreadQueue threadQueue;
	};
	static_assert(sizeof(OSCond) == 0x1C);

	/* OSSemaphore */

	struct OSSemaphore
	{
		uint32be magic;
		MEMPTR<void> userData;
		uint32be ukn08;
		sint32be count;
		OSThreadQueue threadQueue;
	};
	static_assert(sizeof(OSSemaphore) == 0x20);

	/* OSFastCond */

	struct OSFastCond
	{
		uint32be magic;
		MEMPTR<void> userData;
		uint32be ukn08;
		OSThreadQueue threadQueue;
	};
	static_assert(sizeof(OSFastCond) == 0x1C);
};

struct OSThread_t
{
	static constexpr uint32 MAGIC_THREAD = 0x74487244; // 'tHrD'

	enum class THREAD_TYPE : uint32
	{
		TYPE_DRIVER = 0,
		TYPE_IO = 1,
		TYPE_APP = 2
	};

	enum class THREAD_STATE : uint8
	{
		STATE_NONE = 0,     // not started, or finished and released
		STATE_READY = 1,
		STATE_RUNNING = 2,
		STATE_WAITING = 4,
		STATE_MORIBUND = 8, // finished, waiting to be joined
	};

	enum ATTR_BIT : uint32
	{
		ATTR_AFFINITY_CORE0 = 0x1,
		ATTR_AFFINITY_CORE1 = 0x2,
		ATTR_AFFINITY_CORE2 = 0x4,
		ATTR_DETACHED		= 0x8,
		ATTR_UKN_010		= 0x10,
	};

	enum REQUEST_FLAG_BIT : uint32
	{
		REQUEST_FLAG_NONE = 0,
		REQUEST_FLAG_SUSPEND = 1,
		REQUEST_FLAG_CANCEL = 2,
	};

	// thread types use separate priority bands: driver 0-31, I/O 32-63, application 64-95
	static sint32 GetTypePriorityBase(THREAD_TYPE threadType)
	{
		switch (threadType)
		{
		case THREAD_TYPE::TYPE_IO:
			return 32;
		case THREAD_TYPE::TYPE_APP:
			return 64;
		default:
			return 0;
		}
	}

	void SetThreadMagic()
	{
		magic = MAGIC_THREAD;
	}

	bool IsValidMagic() const
	{
		return magic == MAGIC_THREAD && context.magic0 == OSContext_t::OS_CONTEXT_MAGIC_0 && context.magic1 == OSContext_t::OS_CONTEXT_MAGIC_1;
	}

	/* +0x000 */ OSContext_t context;
	/* +0x320 */ uint32be magic;
	/* +0x324 */ betype<THREAD_STATE> state;
	/* +0x325 */ uint8 attr;                       // ATTR_BIT
	/* +0x326 */ uint16be id;                      // some titles (Warriors Orochi 3) identify threads by it
	/* +0x328 */ betype<sint32> suspendCounter;
	/* +0x32C */ sint32be effectivePriority;       // lower value = higher priority
	/* +0x330 */ sint32be basePriority;
	/* +0x334 */ uint32be exitValue;

	/* +0x338 */ MEMPTR<coreinit::OSThreadQueue> currentRunQueue[Espresso::CORE_COUNT]; // run queue per core the thread is on, or null
	/* +0x344 */ coreinit::OSThreadLink linkRun[Espresso::CORE_COUNT];

	/* +0x35C */ MEMPTR<coreinit::OSThreadQueueInternal> currentWaitQueue;
	/* +0x360 */ coreinit::OSThreadLink waitQueueLink;

	/* +0x368 */ coreinit::OSThreadQueue joinQueue;

	/* +0x378 */ MEMPTR<coreinit::OSMutex> waitingForMutex;
	/* +0x37C */ coreinit::OSMutexQueue mutexQueue; // mutexes held by this thread

	/* +0x38C */ coreinit::OSThreadLink activeThreadChain;

	/* +0x394 */ MEMPTR<void> stackBase;            // top of the stack (highest address)
	/* +0x398 */ MEMPTR<void> stackEnd;             // bottom of the stack

	/* +0x39C */ MEMPTR<void> entrypoint;
	/* +0x3A0 */ crt_t crt;

	/* +0x578 */ sint32 alarmRelatedUkn;
	/* +0x57C */ std::array<MEMPTR<void>, 16> specificArray; // OSSetThreadSpecific slots
	/* +0x5BC */ betype<THREAD_TYPE> type;
	/* +0x5C0 */ MEMPTR<const char> threadName;
	/* +0x5C4 */ MEMPTR<void> waitAlarm;

	/* +0x5C8 */ uint32 userStackPointer;

	/* +0x5CC */ MEMPTR<void> cleanupCallback;
	/* +0x5D0 */ MEMPTR<void> deallocatorFunc;

	/* +0x5D4 */ uint32 stateFlags;
	/* +0x5D8 */ betype<REQUEST_FLAG_BIT> requestFlags;
	/* +0x5DC */ sint32 pendingSuspend;
	/* +0x5E0 */ sint32 suspendResult;
	/* +0x5E4 */ coreinit::OSThreadQueue suspendQueue;
	/* +0x5F4 */ uint32 _padding5F4;
	/* +0x5F8 */ uint64be quantumTicks;
	/* +0x600 */ uint64 coretimeSumQuantumStart;

	/* +0x608 */ uint64be wakeUpCount;              // number of timeslices received
	/* +0x610 */ uint64be totalCycles;              // cycles executed in all timeslices
	/* +0x618 */ uint64be wakeUpTime;               // start of the most recent timeslice
	/* +0x620 */ uint64 wakeTimeRelatedUkn1;
	/* +0x628 */ uint64 wakeTimeRelatedUkn2;

	// OSSetExceptionCallback handlers per core
	/* +0x630 */ MPTR dsiCallback[Espresso::CORE_COUNT];
	/* +0x63C */ MPTR isiCallback[Espresso::CORE_COUNT];
	/* +0x648 */ MPTR programCallback[Espresso::CORE_COUNT];
	/* +0x654 */ MPTR perfMonCallback[Espresso::CORE_COUNT];

	/* +0x660 */ uint32 ukn660;

	// thread local storage
	/* +0x664 */ uint16 numAllocatedTLSBlocks;
	/* +0x666 */ sint16 tlsStatus;
	/* +0x668 */ MPTR tlsBlocksMPTR;

	/* +0x66C */ MEMPTR<coreinit::OSFastMutex> waitingForFastMutex;
	/* +0x670 */ coreinit::OSFastMutexLink contendedFastMutex;
	/* +0x678 */ coreinit::OSFastMutexLink ownedFastMutex;
	/* +0x680 */ MEMPTR<void> alignmentExceptionCallback[Espresso::CORE_COUNT];

	/* +0x68C */ uint32 padding68C[20 / 4];
};
static_assert(offsetof(OSThread_t, magic) == 0x320);
static_assert(offsetof(OSThread_t, currentRunQueue) == 0x338);
static_assert(offsetof(OSThread_t, waitQueueLink) == 0x360);
static_assert(offsetof(OSThread_t, mutexQueue) == 0x37C);
static_assert(offsetof(OSThread_t, crt) == 0x3A0);
static_assert(offsetof(OSThread_t, type) == 0x5BC);
static_assert(offsetof(OSThread_t, stateFlags) == 0x5D4);
static_assert(offsetof(OSThread_t, quantumTicks) == 0x5F8);
static_assert(offsetof(OSThread_t, dsiCallback) == 0x630);
static_assert(offsetof(OSThread_t, tlsBlocksMPTR) == 0x668);
static_assert(offsetof(OSThread_t, alignmentExceptionCallback) == 0x680);
static_assert(sizeof(OSThread_t) == 0x6A0);

namespace coreinit
{
	void MapThreadExports();
	void InitializeThread();

	void InitializeConcurrency();

	bool __CemuIsMulticoreMode();

	OSThread_t* OSGetDefaultThread(sint32 coreIndex);
	void* OSGetDefaultThreadStack(sint32 coreIndex, uint32& size);

	bool OSCreateThreadType(OSThread_t* thread, MPTR entryPoint, sint32 numParam, void* ptrParam, void* stackTop2, sint32 stackSize, sint32 priority, uint32 attr, OSThread_t::THREAD_TYPE threadType);
	bool __OSCreateThreadType(OSThread_t* thread, MPTR entryPoint, sint32 numParam, void* ptrParam, void* stackTop, sint32 stackSize, sint32 priority, uint32 attr, OSThread_t::THREAD_TYPE threadType);
	void OSCreateThreadInternal(OSThread_t* thread, uint32 entryPoint, MPTR stackLowerBaseAddr, uint32 stackSize, uint8 affinityMask, OSThread_t::THREAD_TYPE threadType);
	bool OSRunThread(OSThread_t* thread, MPTR funcAddress, sint32 numParam, void* ptrParam);
	void OSExitThread(sint32 exitValue);
	void OSDetachThread(OSThread_t* thread);

	OSThread_t* OSGetCurrentThread();
	void OSSetCurrentThread(uint32 coreIndex, OSThread_t* thread);

	void __OSSetThreadBasePriority(OSThread_t* thread, sint32 newPriority);
	void __OSUpdateThreadEffectivePriority(OSThread_t* thread);

	bool OSSetThreadPriority(OSThread_t* thread, sint32 newPriority);
	uint32 OSGetThreadAffinity(OSThread_t* thread);

	void OSSetThreadName(OSThread_t* thread, const char* name);
	const char* OSGetThreadName(OSThread_t* thread);

	sint32 __OSResumeThreadInternal(OSThread_t* thread, sint32 resumeCount);
	sint32 OSResumeThread(OSThread_t* thread);
	void OSContinueThread(OSThread_t* thread);
	void __OSSuspendThreadInternal(OSThread_t* thread);
	void __OSSuspendThreadNolock(OSThread_t* thread);
	void OSSuspendThread(OSThread_t* thread);
	void OSSleepThread(OSThreadQueue* threadQueue);
	void OSWakeupThread(OSThreadQueue* threadQueue);
	bool OSJoinThread(OSThread_t* thread, uint32be* exitValue);

	void OSTestThreadCancelInternal();

	void OSYieldThread();
	void OSSleepTicks(uint64 ticks);

	bool OSIsThreadTerminated(OSThread_t* thread);
	bool OSIsThreadSuspended(OSThread_t* thread);
	bool OSIsThreadRunningNoLock(OSThread_t* thread);
	bool OSIsThreadRunning(OSThread_t* thread);

	// OSThreadQueue
	void OSInitThreadQueue(OSThreadQueue* threadQueue);
	void OSInitThreadQueueEx(OSThreadQueue* threadQueue, void* userData);

	// OSEvent
	void OSInitEvent(OSEvent* event, OSEvent::EVENT_STATE initialState, OSEvent::EVENT_MODE mode);
	void OSInitEventEx(OSEvent* event, OSEvent::EVENT_STATE initialState, OSEvent::EVENT_MODE mode, void* userData);
	void OSResetEvent(OSEvent* event);
	void OSWaitEventInternal(OSEvent* event); // scheduler lock must be held
	void OSWaitEvent(OSEvent* event);
	bool OSWaitEventWithTimeout(OSEvent* event, uint64 timeout);
	void OSSignalEventInternal(OSEvent* event); // scheduler lock must be held
	void OSSignalEvent(OSEvent* event);
	void OSSignalEventAllInternal(OSEvent* event); // scheduler lock must be held
	void OSSignalEventAll(OSEvent* event);

	// OSRendezvous
	void OSInitRendezvous(OSRendezvous* rendezvous);
	bool OSWaitRendezvous(OSRendezvous* rendezvous, uint32 coreMask);

	// OSMutex
	void OSInitMutex(OSMutex* mutex);
	void OSInitMutexEx(OSMutex* mutex, void* userData);
	void OSLockMutex(OSMutex* mutex);
	bool OSTryLockMutex(OSMutex* mutex);
	void OSUnlockMutex(OSMutex* mutex);
	void OSUnlockMutexInternal(OSMutex* mutex);

	// OSCond
	void OSInitCond(OSCond* cond);
	void OSInitCondEx(OSCond* cond, void* userData);
	void OSSignalCond(OSCond* cond);
	void OSWaitCond(OSCond* cond, OSMutex* mutex);

	// OSSemaphore
	void OSInitSemaphore(OSSemaphore* semaphore, sint32 initialCount);
	void OSInitSemaphoreEx(OSSemaphore* semaphore, sint32 initialCount, void* userData);
	sint32 OSWaitSemaphoreInternal(OSSemaphore* semaphore);
	sint32 OSWaitSemaphore(OSSemaphore* semaphore);
	sint32 OSTryWaitSemaphore(OSSemaphore* semaphore);
	sint32 OSSignalSemaphore(OSSemaphore* semaphore);
	sint32 OSSignalSemaphoreInternal(OSSemaphore* semaphore, bool reschedule);
	sint32 OSGetSemaphoreCount(OSSemaphore* semaphore);

	// OSFastMutex
	void OSFastMutex_Init(OSFastMutex* fastMutex, void* userData);
	void OSFastMutex_Lock(OSFastMutex* fastMutex);
	bool OSFastMutex_TryLock(OSFastMutex* fastMutex);
	void OSFastMutex_Unlock(OSFastMutex* fastMutex);

	// OSFastCond
	void OSFastCond_Init(OSFastCond* fastCond, void* userData);
	void OSFastCond_Wait(OSFastCond* fastCond, OSFastMutex* fastMutex);
	void OSFastCond_Signal(OSFastCond* fastCond);

	// scheduler
	void OSSchedulerBegin(sint32 numCPUEmulationThreads);
	void OSSchedulerEnd();
	bool OSIsSchedulerActive();

	// internal
	void __OSAddReadyThreadToRunQueue(OSThread_t* thread);
	bool __OSCoreShouldSwitchToThread(OSThread_t* currentThread, OSThread_t* newThread, bool sharedPriorityAndAffinityWorkaround);
	void __OSQueueThreadDeallocation(OSThread_t* thread);

	bool __OSIsThreadActive(OSThread_t* thread);
	void __OSDeleteAllActivePPCThreads();
}

#pragma pack()

// table of active guest threads for the debugger views, guarded by srwlock_activeThreadList
extern MPTR activeThread[256];
extern sint32 activeThreadCount;

extern SlimRWLock srwlock_activeThreadList;
