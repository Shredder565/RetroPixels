// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// proc_ui sits between a title and the system message queue. The system tells
// the foreground title when it must give up the screen (HOME menu, shutdown),
// when it gets it back, and when to exit. ProcUIProcessMessages reads those
// messages and returns the state the title is in; around each transition it
// runs the callbacks the title registered.
//
// Callbacks are registered per CPU core and must run on that core, so we keep
// one worker thread per core. A transition is handed to all workers at once;
// each runs its core's callback list and reports back, and the caller only
// continues once every core is done.
//
// Releasing the foreground is two-step: the release message only moves us to
// "Releasing"; the title finishes its frame, calls ProcUIDrawDoneRelease, and
// the next ProcUIProcessMessages does the actual hand-over (release callbacks,
// save callback, then the per-core OSReleaseForeground acknowledgements).
// While in the background a blocking caller sleeps until the system sends
// "acquire foreground" or "exit"; a helper thread on core 2 waits for that and
// still serves network start/stop notifications in the meantime.
//
// The library also registers an OS driver so MEM1 and the foreground bucket,
// which other processes may use while we are in the background, are saved
// into and restored from buffers the title provided.
//
// Some titles skip proc_ui and read OSGetSystemMessageQueue() themselves.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"
#include "Cafe/OS/libs/coreinit/coreinit_Memory.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"
#include "Cafe/OS/libs/gx2/GX2_Misc.h"
#include "Cafe/OS/RPL/rpl.h"
#include "proc_ui.h"

namespace proc_ui
{
	using namespace coreinit;

	namespace
	{
		constexpr sint32 kCores = Espresso::CORE_COUNT;
		constexpr sint32 kKinds = (sint32)ProcUICallbackId::COUNT;
		constexpr sint32 kMessageCore = 2; // background alarms and the message helper live here
		constexpr uint32 kDefaultStackSize = 0x2000;
		constexpr sint32 kHelperThreadPriority = 16;
		constexpr uint32 kSdkRegisterBeforeInit = 21102; // older SDKs allowed callbacks before ProcUIInit
		constexpr uint32 kCalcBytesPerCallback = 0x8C0;   // what the system library reserves per callback
		constexpr uint32 kCalcExtraBytes = 100;

		// Allocated from the title's heap. The alarm must stay the first member:
		// the alarm handler gets the alarm pointer and treats it as the node.
		struct CallbackNode
		{
			OSAlarm_t alarm;
			uint64be period;
			MEMPTR<void> func;
			MEMPTR<void> arg;
			sint32be order;
			MEMPTR<CallbackNode> next;
		};

		// what the per-core workers are asked to do
		enum class WorkerJob : uint32
		{
			AcquireForeground = 0, // values match ProcUICallbackId
			ReleaseForeground = 1,
			Exit = 2,
			NetIoStart = 3,
			NetIoStop = 4,
			HomeButtonDenied = 5,
			Quit = 100, // leave the thread
		};

		// everything the guest's OS functions need to see lives in guest memory
		struct GuestObjects
		{
			OSThread_t worker[kCores];
			OSThread_t messageHelper;
			OSEvent wake[kCores];        // auto reset: one job for this core
			OSEvent finished[kCores];    // auto reset: this core ran its callbacks
			OSEvent releaseGate;         // manual: workers acknowledge the release after this
			OSEvent stateChanged;        // manual: set on acquire/exit, cleared on release
			OSEvent helperGotMessage;    // manual: the background helper has a message
			OSMessage helperMessage;
			char workerName[kCores][24];
			char driverName[16];
			OSDriverInterface driver;
		};
		SysAllocator<GuestObjects> s_guest;

		struct LibState
		{
			std::atomic_bool initialized{false};
			bool foreground = false;
			bool shuttingDown = false;
			bool isForegroundProcess = true;
			bool helperRunning = false;
			bool drawDoneRelease = false;
			ProcUIStatus status = ProcUIStatus::Foreground;

			MEMPTR<void> saveCallback;      // ProcUIInit: void(void)
			MEMPTR<void> saveCallbackEx;    // ProcUIInitEx / ProcUISetSaveCallback: uint32(void*)
			MEMPTR<void> saveCallbackExArg;
			MEMPTR<OSMessageQueue> systemQueue;

			// allocator used for nodes and stacks (guest functions)
			MEMPTR<void> allocFn;
			MEMPTR<void> freeFn;
			MEMPTR<void> poolHeap;

			uint32 stackSize = 0;
			bool workersRunning = false;
			std::atomic<WorkerJob> job{WorkerJob::Quit};
			MEMPTR<void> workerStack[kCores];
			MEMPTR<void> helperStack;

			MEMPTR<CallbackNode> lists[kKinds][kCores];
			MEMPTR<CallbackNode> backgroundList;

			MEMPTR<void> mem1Backup;
			MEMPTR<void> bucketBackup;

			bool driverActive = false;
			bool driverStartedInBackground = false;
			uint32be driverOut1;
			uint32be driverOut2;
		};
		LibState s;

		// --- memory ---------------------------------------------------------

		void UseDefaultHeap()
		{
			s.allocFn = gCoreinitData->MEMAllocFromDefaultHeap;
			s.freeFn = gCoreinitData->MEMFreeToDefaultHeap;
		}

		void* Allocate(uint32 size)
		{
			return MEMPTR<void>(PPCCoreCallback(s.allocFn, size)).GetPtr();
		}

		void Release(MEMPTR<void>& block)
		{
			if (block)
				PPCCoreCallback(s.freeFn, block);
			block = nullptr;
		}

		void* PoolAlloc(uint32 size)
		{
			return MEMAllocFromExpHeapEx((MEMHeapHandle)s.poolHeap.GetPtr(), size, 4);
		}

		void PoolFree(void* block)
		{
			MEMFreeToExpHeap((MEMHeapHandle)s.poolHeap.GetPtr(), block);
		}

		// --- callback lists ---------------------------------------------------

		void InsertSorted(MEMPTR<CallbackNode>& head, void* func, void* arg, uint64 period, sint32 order)
		{
			auto* node = (CallbackNode*)Allocate(sizeof(CallbackNode));
			if (!node)
			{
				cemuLog_log(LogType::APIErrors, "ProcUI: out of memory while registering a callback");
				return;
			}
			memset((void*)node, 0, sizeof(CallbackNode));
			node->func = func;
			node->arg = arg;
			node->period = period;
			node->order = order;
			// keep the list ordered by 'order'; equal entries stay in registration order
			MEMPTR<CallbackNode>* link = &head;
			while (*link && (sint32)(*link)->order <= order)
				link = &(*link)->next;
			node->next = *link;
			*link = node;
		}

		void FreeList(MEMPTR<CallbackNode>& head)
		{
			MEMPTR<CallbackNode> cur = head;
			head = nullptr;
			while (cur)
			{
				MEMPTR<CallbackNode> next = cur->next;
				MEMPTR<void> block = cur.GetPtr();
				Release(block);
				cur = next;
			}
		}

		void ForgetAllLists()
		{
			for (auto& kind : s.lists)
				for (auto& perCore : kind)
					perCore = nullptr;
			s.backgroundList = nullptr;
		}

		void RunList(CallbackNode* node)
		{
			for (; node; node = node->next.GetPtr())
			{
				uint32 err = PPCCoreCallback(node->func, node->arg);
				if (err != 0)
					cemuLog_log(LogType::APIErrors, "ProcUI: callback {:08x} returned {}", node->func.GetMPTR(), err);
			}
		}

		void OnBackgroundAlarm(OSAlarm_t* alarm, void* context)
		{
			auto* node = (CallbackNode*)alarm;
			uint32 err = PPCCoreCallback(node->func, node->arg);
			if (err != 0)
				cemuLog_log(LogType::APIErrors, "ProcUI: background callback {:08x} returned {}", node->func.GetMPTR(), err);
		}

		void ArmBackgroundAlarms()
		{
			static const MPTR handler = makeCallableExport<&OnBackgroundAlarm>();
			for (CallbackNode* n = s.backgroundList.GetPtr(); n; n = n->next.GetPtr())
			{
				OSCreateAlarm(&n->alarm);
				OSSetPeriodicAlarm(&n->alarm, OSGetTime(), n->period, handler);
			}
		}

		void DisarmBackgroundAlarms()
		{
			for (CallbackNode* n = s.backgroundList.GetPtr(); n; n = n->next.GetPtr())
				OSCancelAlarm(&n->alarm);
		}

		// --- per-core workers -------------------------------------------------

		uint32 WorkerMain(uint32 core, void* unused)
		{
			GuestObjects& g = *s_guest.GetPtr();
			while (true)
			{
				OSWaitEvent(&g.wake[core]);
				const WorkerJob job = s.job.load();
				if (job == WorkerJob::Quit)
					return 0;
				if (core == kMessageCore)
				{
					if (job == WorkerJob::AcquireForeground)
						DisarmBackgroundAlarms();
					else if (job == WorkerJob::ReleaseForeground)
						ArmBackgroundAlarms();
				}
				if ((uint32)job < (uint32)kKinds)
					RunList(s.lists[(uint32)job][core].GetPtr());
				OSSignalEvent(&g.finished[core]);
				if (job == WorkerJob::ReleaseForeground)
				{
					// every core acknowledges the release once the title has saved
					OSWaitEvent(&g.releaseGate);
					OSReleaseForeground();
				}
			}
		}

		// Hands a job to every core and waits until each one finished its list.
		void RunOnAllCores(WorkerJob job)
		{
			GuestObjects& g = *s_guest.GetPtr();
			s.job.store(job);
			OSMemoryBarrier();
			for (sint32 c = 0; c < kCores; c++)
				OSSignalEvent(&g.wake[c]);
			for (sint32 c = 0; c < kCores; c++)
				OSWaitEvent(&g.finished[c]);
		}

		void StopWorkers()
		{
			if (!s.workersRunning)
				return;
			GuestObjects& g = *s_guest.GetPtr();
			s.job.store(WorkerJob::Quit);
			OSMemoryBarrier();
			for (sint32 c = 0; c < kCores; c++)
				OSSignalEvent(&g.wake[c]);
			for (sint32 c = 0; c < kCores; c++)
				OSJoinThread(&g.worker[c], nullptr);
			// registered callbacks stay; ProcUIShutdown frees them before stopping us
			for (auto& stack : s.workerStack)
				Release(stack);
			Release(s.helperStack);
			s.workersRunning = false;
		}

		void StartWorkers()
		{
			static const MPTR entry = makeCallableExport<&WorkerMain>();
			StopWorkers();
			GuestObjects& g = *s_guest.GetPtr();
			for (sint32 c = 0; c < kCores; c++)
			{
				OSInitEvent(&g.wake[c], OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, OSEvent::EVENT_MODE::MODE_AUTO);
				OSInitEvent(&g.finished[c], OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, OSEvent::EVENT_MODE::MODE_AUTO);
				s.workerStack[c] = Allocate(s.stackSize);
			}
			s.helperStack = Allocate(s.stackSize);
			for (sint32 c = 0; c < kCores; c++)
			{
				uint8* stackTop = (uint8*)s.workerStack[c].GetPtr() + s.stackSize;
				__OSCreateThreadType(&g.worker[c], entry, c, nullptr, stackTop, s.stackSize, kHelperThreadPriority, 1u << c, OSThread_t::THREAD_TYPE::TYPE_DRIVER);
				snprintf(g.workerName[c], sizeof(g.workerName[c]), "{SYS ProcUI Core %d}", c);
				OSSetThreadName(&g.worker[c], g.workerName[c]);
				OSResumeThread(&g.worker[c]);
			}
			s.workersRunning = true;
		}

		// --- background message helper ----------------------------------------

		uint32 MessageHelperMain(uint32 unused0, void* unused1)
		{
			GuestObjects& g = *s_guest.GetPtr();
			while (true)
			{
				OSReceiveMessage(s.systemQueue, &g.helperMessage, OS_MESSAGE_BLOCK);
				auto id = (SysMessageId)(uint32)g.helperMessage.data0;
				if (id == SysMessageId::MsgAcquireForeground || id == SysMessageId::MsgExit)
					break;
				if (id == SysMessageId::NetIoStartOrStop)
				{
					const sint32 kind = g.helperMessage.data1 != 0 ? (sint32)ProcUICallbackId::NetIoStart : (sint32)ProcUICallbackId::NetIoStop;
					for (sint32 c = 0; c < kCores; c++)
						RunList(s.lists[kind][c].GetPtr());
				}
				else
					cemuLog_log(LogType::Force, "ProcUI: unexpected system message {:08x} while in background", (uint32)id);
			}
			OSSignalEvent(&g.helperGotMessage);
			return 0;
		}

		void StartMessageHelper()
		{
			static const MPTR entry = makeCallableExport<&MessageHelperMain>();
			GuestObjects& g = *s_guest.GetPtr();
			OSResetEvent(&g.helperGotMessage);
			uint8* stackTop = (uint8*)s.helperStack.GetPtr() + s.stackSize;
			__OSCreateThreadType(&g.messageHelper, entry, 0, nullptr, stackTop, s.stackSize, kHelperThreadPriority, 1u << kMessageCore, OSThread_t::THREAD_TYPE::TYPE_DRIVER);
			OSResumeThread(&g.messageHelper);
			s.helperRunning = true;
		}

		// --- transitions -----------------------------------------------------------

		// Acts on one system message. Returns false for the exit message.
		bool HandleSystemMessage(const OSMessage& msg)
		{
			GuestObjects& g = *s_guest.GetPtr();
			const auto id = (SysMessageId)(uint32)msg.data0;
			switch (id)
			{
			case SysMessageId::MsgAcquireForeground:
				s.shuttingDown = false;
				RunOnAllCores(WorkerJob::AcquireForeground);
				s.status = ProcUIStatus::Foreground;
				s.foreground = true;
				OSMemoryBarrier();
				OSSignalEvent(&g.stateChanged);
				return true;
			case SysMessageId::MsgExit:
				s.shuttingDown = true;
				RunOnAllCores(WorkerJob::Exit);
				for (sint32 c = 0; c < kCores; c++)
					FreeList(s.lists[(sint32)ProcUICallbackId::Exit][c]);
				s.status = ProcUIStatus::Exit;
				OSMemoryBarrier();
				OSSignalEvent(&g.stateChanged);
				return false;
			case SysMessageId::MsgReleaseForeground:
				// a non-zero argument means the release is the start of a shutdown
				if (msg.data1 != 0)
					s.shuttingDown = true;
				s.status = ProcUIStatus::Releasing;
				OSResetEvent(&g.stateChanged);
				// the hand-over waits for ProcUIDrawDoneRelease
				return true;
			case SysMessageId::HomeButtonDenied:
				RunOnAllCores(WorkerJob::HomeButtonDenied);
				return true;
			case SysMessageId::NetIoStartOrStop:
				RunOnAllCores(msg.data1 != 0 ? WorkerJob::NetIoStart : WorkerJob::NetIoStop);
				return true;
			default:
				cemuLog_log(LogType::Force, "ProcUI: unknown system message {:08x}", (uint32)id);
				return true;
			}
		}

		void FinishRelease(bool blockingInBackground)
		{
			GuestObjects& g = *s_guest.GetPtr();
			s.foreground = false;
			s.status = ProcUIStatus::Background;
			OSResetEvent(&g.releaseGate);
			RunOnAllCores(WorkerJob::ReleaseForeground);
			if (s.saveCallback)
				PPCCoreCallback(s.saveCallback);
			if (s.saveCallbackEx)
				PPCCoreCallback(s.saveCallbackEx, s.saveCallbackExArg);
			if (s.isForegroundProcess && blockingInBackground)
				StartMessageHelper();
			OSSignalEvent(&g.releaseGate);
			s.drawDoneRelease = false;
		}

		// --- OS driver: keep MEM1 and the foreground bucket across background time

		void CopyArea(bool save, bool mem1)
		{
			MEMPTR<void> areaBase;
			uint32be areaSize;
			if (mem1)
				OSGetMemBound(1, &areaBase, &areaSize);
			else
				OSGetForegroundBucketFreeArea(&areaBase, &areaSize);
			MEMPTR<void> backup = mem1 ? s.mem1Backup : s.bucketBackup;
			if (!backup)
				return;
			if (save)
				OSBlockMove(backup, areaBase, areaSize, true);
			else
			{
				OSBlockMove(areaBase, backup, areaSize, true);
				GX2::GX2Invalidate(GX2::GX2InvalidationFlag::CPU, areaBase.GetMPTR(), areaSize);
			}
		}

		const char* DriverName()
		{
			GuestObjects& g = *s_guest.GetPtr();
			strcpy(g.driverName, "ProcUI");
			return g.driverName;
		}

		void DriverInit()
		{
			s.driverActive = true;
			OSMemoryBarrier();
		}

		void DriverAcquiredForeground()
		{
			if (s.driverStartedInBackground)
			{
				DriverInit();
				s.driverStartedInBackground = false;
				return;
			}
			CopyArea(false, true);
			CopyArea(false, false);
			s.driverActive = true;
			OSMemoryBarrier();
		}

		void DriverReleaseForeground()
		{
			CopyArea(true, true);
			CopyArea(true, false);
			s.driverActive = false;
			OSMemoryBarrier();
		}

		void DriverDone()
		{
			if (!s.driverActive)
				return;
			ProcUIShutdown();
			s.driverActive = false;
			OSMemoryBarrier();
		}

		void ResetState()
		{
			// back to "library just loaded"
			s.initialized = false;
			s.foreground = false; // ProcUIInForeground is false until ProcUIInit
			s.shuttingDown = false;
			s.isForegroundProcess = true;
			s.helperRunning = false;
			s.drawDoneRelease = false;
			s.status = ProcUIStatus::Foreground;
			s.saveCallback = nullptr;
			s.saveCallbackEx = nullptr;
			s.saveCallbackExArg = nullptr;
			s.systemQueue = nullptr;
			s.allocFn = nullptr;
			s.freeFn = nullptr;
			s.poolHeap = nullptr;
			s.stackSize = 0;
			s.workersRunning = false;
			s.job = WorkerJob::Quit;
			for (auto& stack : s.workerStack)
				stack = nullptr;
			s.helperStack = nullptr;
			ForgetAllLists();
			s.mem1Backup = nullptr;
			s.bucketBackup = nullptr;
			s.driverActive = false;
			s.driverStartedInBackground = false;
		}

		void InitCommon()
		{
			if (s.initialized.exchange(true))
				return;
			GuestObjects& g = *s_guest.GetPtr();
			if (!s.poolHeap)
				UseDefaultHeap();
			OSInitEvent(&g.stateChanged, OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, OSEvent::EVENT_MODE::MODE_MANUAL);
			OSInitEvent(&g.releaseGate, OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, OSEvent::EVENT_MODE::MODE_MANUAL);
			OSInitEvent(&g.helperGotMessage, OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, OSEvent::EVENT_MODE::MODE_MANUAL);
			s.status = ProcUIStatus::Foreground;
			s.foreground = true;
			s.drawDoneRelease = false;
			s.helperRunning = false;
			s.stackSize = kDefaultStackSize;
			s.systemQueue = OSGetSystemMessageQueue();
			// the HOME Menu (2) and games (15) both run as the foreground process
			const uint32 upid = OSGetUPID();
			s.isForegroundProcess = upid == 2 || upid == 15;
			StartWorkers();
			ForgetAllLists();
		}
	}

	// --- exports ---------------------------------------------------------------

	void ProcUIInit(MEMPTR<void> callbackReadyToRelease)
	{
		s.saveCallback = callbackReadyToRelease;
		s.saveCallbackEx = nullptr;
		s.saveCallbackExArg = nullptr;
		InitCommon();
	}

	void ProcUIInitEx(MEMPTR<void> callbackReadyToReleaseEx, MEMPTR<void> userParam)
	{
		s.saveCallback = nullptr;
		s.saveCallbackEx = callbackReadyToReleaseEx;
		s.saveCallbackExArg = userParam;
		InitCommon();
	}

	void ProcUIShutdown()
	{
		if (!s.initialized.exchange(false))
			return;
		if (!OSIsSchedulerActive())
			return; // emulation is stopping; ResetState cleans up on the next load
		if (!s.foreground)
			DisarmBackgroundAlarms();
		GuestObjects& g = *s_guest.GetPtr();
		for (sint32 c = 0; c < kCores; c++)
			OSSetThreadPriority(&g.worker[c], 0);
		RunOnAllCores(WorkerJob::Exit);
		ProcUIClearCallbacks();
		StopWorkers();
	}

	bool ProcUIIsRunning()
	{
		return s.initialized;
	}

	bool ProcUIInForeground()
	{
		return s.foreground;
	}

	bool ProcUIInShutdown()
	{
		return s.shuttingDown;
	}

	void ProcUIRegisterCallbackCore(ProcUICallbackId callbackType, void* funcPtr, void* userParam, sint32 priority, uint32 coreIndex)
	{
		if ((uint32)callbackType >= (uint32)kKinds || coreIndex >= (uint32)kCores)
		{
			cemuLog_log(LogType::Force, "ProcUIRegisterCallback: bad callback type {} or core {}", (uint32)callbackType, coreIndex);
			return;
		}
		if (__OSGetProcessSDKVersion() < kSdkRegisterBeforeInit)
		{
			if (!s.allocFn)
				UseDefaultHeap();
		}
		else if (!s.initialized)
		{
			cemuLog_log(LogType::Force, "ProcUI: callback registered before ProcUIInit");
			if (!s.allocFn)
				UseDefaultHeap();
		}
		// acquire callbacks run lowest priority value first, all others highest first
		const sint32 order = callbackType == ProcUICallbackId::AcquireForeground ? priority : -priority;
		InsertSorted(s.lists[(uint32)callbackType][coreIndex], funcPtr, userParam, 0, order);
	}

	void ProcUIRegisterCallback(ProcUICallbackId callbackType, void* funcPtr, void* userParam, sint32 priority)
	{
		ProcUIRegisterCallbackCore(callbackType, funcPtr, userParam, priority, OSGetCoreId());
	}

	void ProcUIRegisterBackgroundCallback(void* funcPtr, void* userParam, uint64 tickDelay)
	{
		if (!s.allocFn)
			UseDefaultHeap();
		InsertSorted(s.backgroundList, funcPtr, userParam, tickDelay, 0);
	}

	void ProcUIClearCallbacks()
	{
		for (auto& kind : s.lists)
			for (auto& perCore : kind)
				FreeList(perCore);
		if (!s.foreground)
			DisarmBackgroundAlarms();
		FreeList(s.backgroundList);
	}

	void ProcUISetSaveCallback(void* funcPtr, void* userParam)
	{
		s.saveCallback = nullptr;
		s.saveCallbackEx = funcPtr;
		s.saveCallbackExArg = userParam;
	}

	void ProcUISetCallbackStackSize(uint32 newStackSize)
	{
		s.stackSize = newStackSize;
		if (s.initialized)
			StartWorkers();
	}

	uint32 ProcUICalcMemorySize(uint32 numCallbacks)
	{
		// one stack per core worker plus one for the background message helper
		return (kCores + 1) * s.stackSize + kCalcBytesPerCallback * numCallbacks + kCalcExtraBytes;
	}

	sint32 ProcUISetMemoryPool(void* memBase, uint32 size)
	{
		static const MPTR allocFn = makeCallableExport<&PoolAlloc>();
		static const MPTR freeFn = makeCallableExport<&PoolFree>();
		s.allocFn = MEMPTR<void>(allocFn);
		s.freeFn = MEMPTR<void>(freeFn);
		s.poolHeap = MEMCreateExpHeapEx(memBase, size, MEM_HEAP_OPTION_THREADSAFE);
		return s.poolHeap ? 0 : -1;
	}

	void ProcUISetBucketStorage(void* memBase, uint32 size)
	{
		MEMPTR<void> base;
		uint32be freeSize;
		OSGetForegroundBucketFreeArea(&base, &freeSize);
		if ((uint32)freeSize < size)
			cemuLog_log(LogType::Force, "ProcUISetBucketStorage: buffer is smaller than the foreground bucket");
		s.bucketBackup = memBase;
	}

	void ProcUISetMEM1Storage(void* memBase, uint32 size)
	{
		MEMPTR<void> base;
		uint32be mem1Size;
		OSGetMemBound(1, &base, &mem1Size);
		if ((uint32)mem1Size < size)
			cemuLog_log(LogType::Force, "ProcUISetMEM1Storage: buffer is smaller than MEM1");
		s.mem1Backup = memBase;
	}

	void ProcUIDrawDoneRelease()
	{
		s.drawDoneRelease = true;
	}

	ProcUIStatus ProcUIProcessMessages(bool isBlockingInBackground)
	{
		if (!s.initialized)
		{
			cemuLog_logOnce(LogType::Force, "ProcUIProcessMessages called before ProcUIInit");
			return ProcUIStatus::Foreground;
		}
		if (!isBlockingInBackground && OSGetCoreId() != kMessageCore)
			cemuLog_logOnce(LogType::Force, "ProcUIProcessMessages: the non-blocking variant is meant to run on core 2");

		s.status = s.foreground ? ProcUIStatus::Foreground : ProcUIStatus::Background;
		if (s.drawDoneRelease)
			FinishRelease(isBlockingInBackground);

		OSMessage msg;
		if (s.foreground || !isBlockingInBackground)
		{
			if (OSReceiveMessage(s.systemQueue, &msg, 0) && !HandleSystemMessage(msg))
				return s.status;
		}

		GuestObjects& g = *s_guest.GetPtr();
		while (!s.foreground && isBlockingInBackground)
		{
			if (!s.isForegroundProcess)
			{
				OSReceiveMessage(s.systemQueue, &msg, OS_MESSAGE_BLOCK);
				if (!HandleSystemMessage(msg))
					return s.status;
				continue;
			}
			if (!s.helperRunning)
				StartMessageHelper(); // the release happened in a non-blocking call
			OSWaitEvent(&g.helperGotMessage);
			OSResetEvent(&g.helperGotMessage);
			OSJoinThread(&g.messageHelper, nullptr);
			s.helperRunning = false;
			msg = g.helperMessage;
			if (!HandleSystemMessage(msg))
				return s.status;
		}
		return s.status;
	}

	ProcUIStatus ProcUISubProcessMessages(bool isBlockingInBackground)
	{
		if (isBlockingInBackground)
		{
			while (s.status == ProcUIStatus::Background)
				OSWaitEvent(&s_guest->stateChanged);
		}
		return s.status;
	}

	class ProcUIModule : public COSModule
	{
	public:
		std::string_view GetName() override { return "proc_ui"; }

		void RPLMapped() override
		{
			ResetState();
			cafeExportRegister("proc_ui", ProcUIInit, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIInitEx, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIShutdown, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIIsRunning, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIInForeground, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIInShutdown, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIRegisterCallbackCore, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIRegisterCallback, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIRegisterBackgroundCallback, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIClearCallbacks, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUISetSaveCallback, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUISetCallbackStackSize, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUICalcMemorySize, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUISetMemoryPool, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUISetBucketStorage, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUISetMEM1Storage, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIDrawDoneRelease, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUIProcessMessages, LogType::ProcUi);
			cafeExportRegister("proc_ui", ProcUISubProcessMessages, LogType::ProcUi);
		}

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == RplEntryReason::Loaded)
			{
				OSDriverInterface& drv = s_guest->driver;
				drv.getDriverName = MEMPTR<void>(makeCallableExport<&DriverName>());
				drv.init = MEMPTR<void>(makeCallableExport<&DriverInit>());
				drv.onAcquireForeground = MEMPTR<void>(makeCallableExport<&DriverAcquiredForeground>());
				drv.onReleaseForeground = MEMPTR<void>(makeCallableExport<&DriverReleaseForeground>());
				drv.done = MEMPTR<void>(makeCallableExport<&DriverDone>());
				s.driverActive = false;
				s.driverStartedInBackground = false;
				s.driverOut1 = 0;
				s.driverOut2 = 0;
				uint32be loadedInForegroundSession = 0;
				OSDriver_Register(moduleHandle, 200, &drv, 0, &s.driverOut1, &s.driverOut2, &loadedInForegroundSession);
				if (loadedInForegroundSession != 0)
				{
					if (OSGetForegroundBucket(nullptr, nullptr))
						DriverInit();
					else
						s.driverStartedInBackground = true;
				}
				OSMemoryBarrier();
			}
			else if (reason == RplEntryReason::Unloaded)
			{
				DriverDone();
				OSDriver_Deregister(moduleHandle, 0);
			}
		}
	};

	ProcUIModule s_procUIModule;

	COSModule* GetModule()
	{
		return &s_procUIModule;
	}
}
