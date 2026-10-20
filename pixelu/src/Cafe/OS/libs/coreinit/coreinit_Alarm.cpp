// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/OS/RPL/rpl.h"

/* Alarms
 *
 * Two layers:
 * - Host alarms (OSHostAlarm) are emulator timers ordered by fire time. The scheduler calls
 *   alarm_update(), which fires every due host alarm while holding the scheduler lock.
 * - Guest alarms (OSAlarm_t) each own one host alarm while armed. When it fires it only wakes
 *   the guest alarm thread, which then runs the alarm handlers (or moves the alarm into its
 *   alarm queue) in guest context, earliest alarm first. */

namespace coreinit
{
	SysAllocator<OSEvent> g_alarmEvent;
	SysAllocator<OSThread_t> g_alarmThread;
	SysAllocator<uint8, 1024 * 128> g_alarmThreadStack;
	SysAllocator<char, 32> g_alarmThreadName;

	/* Host alarms */

	class OSHostAlarm
	{
	public:
		using Callback = void(*)(uint64 currentTick, void* context);

		OSHostAlarm(uint64 fireTime, uint64 period, Callback callback, void* context)
			: m_fireTime(fireTime), m_period(period), m_callback(callback), m_context(context)
		{
			cemu_assert_debug(__OSHasSchedulerLock());
			m_seq = s_nextSeq++;
			s_pending.emplace(Key(), this);
			m_pending = true;
			RefreshEarliest();
		}

		~OSHostAlarm()
		{
			cemu_assert_debug(__OSHasSchedulerLock());
			if (m_pending)
			{
				s_pending.erase(Key());
				RefreshEarliest();
			}
		}

		// fires everything that is due at 'now'; periodic alarms are put back with their next time
		static void FireDue(uint64 now)
		{
			cemu_assert_debug(__OSHasSchedulerLock());
			while (!s_pending.empty())
			{
				auto first = s_pending.begin();
				OSHostAlarm* alarm = first->second;
				if (alarm->m_fireTime > now)
					break;
				s_pending.erase(first);
				alarm->m_pending = false;
				alarm->m_callback(now, alarm->m_context);
				if (alarm->m_period != 0)
				{
					alarm->m_fireTime += alarm->m_period;
					s_pending.emplace(alarm->Key(), alarm);
					alarm->m_pending = true;
				}
				RefreshEarliest();
			}
		}

		// lock-free early out for the scheduler
		static bool AnyDue(uint64 now)
		{
			return now >= s_earliest.load(std::memory_order_relaxed);
		}

		static void ForgetAll()
		{
			s_pending.clear();
			s_earliest = std::numeric_limits<uint64>::max();
		}

	private:
		using SortKey = std::pair<uint64, uint64>; // (fire time, creation order)

		SortKey Key() const
		{
			return { m_fireTime, m_seq };
		}

		static void RefreshEarliest()
		{
			s_earliest = s_pending.empty() ? std::numeric_limits<uint64>::max() : s_pending.begin()->first.first;
		}

		uint64 m_fireTime;
		uint64 m_period;
		uint64 m_seq;
		Callback m_callback;
		void* m_context;
		bool m_pending = false;

		static inline std::map<SortKey, OSHostAlarm*> s_pending;
		static inline std::atomic<uint64> s_earliest{ 0 };
		static inline uint64 s_nextSeq = 0;
	};

	OSHostAlarm* OSHostAlarmCreate(uint64 nextFire, uint64 period, void(*callbackFunc)(uint64 currentTick, void* context), void* context)
	{
		return new OSHostAlarm(nextFire, period, callbackFunc, context);
	}

	void OSHostAlarmDestroy(OSHostAlarm* hostAlarm)
	{
		delete hostAlarm;
	}

	void alarm_update()
	{
		cemu_assert_debug(!__OSHasSchedulerLock());
		const uint64 now = OSGetTime();
		if (!OSHostAlarm::AnyDue(now))
			return;
		__OSLockScheduler();
		OSHostAlarm::FireDue(now);
		__OSUnlockScheduler();
	}

	/* Guest alarms */

	namespace
	{
		constexpr uint32 ALARM_QUEUE_MAGIC = 0x614C6D51; // 'aLmQ'
		constexpr uint32 GROUP_INVALID = 0xFFFFFFFF;

		// armed guest alarms and the host alarm behind each of them
		std::unordered_map<OSAlarm_t*, OSHostAlarm*> s_armed;

		void WakeAlarmThread(uint64 currentTick, void* context)
		{
			OSSignalEventInternal(g_alarmEvent.GetPtr());
		}

		void Disarm(std::unordered_map<OSAlarm_t*, OSHostAlarm*>::iterator it)
		{
			OSHostAlarmDestroy(it->second);
			s_armed.erase(it);
		}

		bool WaitsInQueue(OSAlarm_t* alarm)
		{
			OSAlarmQueue* queue = alarm->alarmQueue.GetPtr();
			return queue && (queue->head.GetPtr() == alarm || alarm->prev);
		}

		void QueuePush(OSAlarm_t* alarm)
		{
			OSAlarmQueue* queue = alarm->alarmQueue.GetPtr();
			alarm->next = nullptr;
			alarm->prev = queue->tail;
			if (queue->tail)
				queue->tail->next = alarm;
			else
				queue->head = alarm;
			queue->tail = alarm;
			queue->threadQueue.wakeupEntireWaitQueue(false);
		}

		void QueueRemove(OSAlarm_t* alarm)
		{
			OSAlarmQueue* queue = alarm->alarmQueue.GetPtr();
			OSAlarm_t* before = alarm->prev.GetPtr();
			OSAlarm_t* after = alarm->next.GetPtr();
			if (after)
				after->prev = before;
			else
				queue->tail = before;
			if (before)
				before->next = after;
			else
				queue->head = after;
			alarm->prev = nullptr;
			alarm->next = nullptr;
		}

		// (re)arms an alarm; periodic alarms whose start lies in the past continue on their period grid
		void Arm(OSAlarm_t* alarm, uint64 start, uint64 period, MPTR handler, bool periodic, OSAlarmQueue* queue)
		{
			cemu_assert_debug(__OSHasSchedulerLock());
			if (WaitsInQueue(alarm))
				return; // a fired alarm must be taken out of its queue first
			uint64 fireTime = start;
			if (periodic)
			{
				if ((sint64)period <= 0)
				{
					cemuLog_log(LogType::APIErrors, "OSSetPeriodicAlarm(): the period must be positive, alarm not set");
					return;
				}
				const uint64 now = OSGetTime();
				if (start < now)
					fireTime = start + ((now - start) / period + 1) * period;
				alarm->startTime = start;
				alarm->nextTime = fireTime;
				alarm->period = period;
			}
			else
			{
				alarm->nextTime = start;
				alarm->period = 0;
			}
			alarm->handler = handler;
			alarm->alarmQueue = queue;

			auto it = s_armed.find(alarm);
			if (it != s_armed.end())
				Disarm(it);
			s_armed[alarm] = OSHostAlarmCreate(fireTime, period, WakeAlarmThread, nullptr);
		}

		// earliest armed alarm that is due at 'now', or null
		OSAlarm_t* EarliestDue(uint64 now)
		{
			OSAlarm_t* best = nullptr;
			for (auto& [alarm, host] : s_armed)
			{
				const uint64 t = alarm->nextTime;
				if (t > now)
					continue;
				if (!best || t < (uint64)best->nextTime || (t == (uint64)best->nextTime && alarm < best))
					best = alarm;
			}
			return best;
		}
	}

	void OSInitAlarmQueue(OSAlarmQueue* queue)
	{
		queue->magic = ALARM_QUEUE_MAGIC;
		queue->name = nullptr;
		queue->ukn08 = 0;
		OSInitThreadQueueEx(&queue->threadQueue, queue);
		queue->head = nullptr;
		queue->tail = nullptr;
	}

	void OSInitAlarmQueueEx(OSAlarmQueue* queue, const char* name)
	{
		OSInitAlarmQueue(queue);
		queue->name = name;
	}

	void OSCreateAlarm(OSAlarm_t* alarm)
	{
		memset(alarm, 0, sizeof(OSAlarm_t));
		alarm->setMagic();
	}

	void OSCreateAlarmEx(OSAlarm_t* alarm, const char* alarmName)
	{
		OSCreateAlarm(alarm);
		alarm->name = alarmName;
	}

	// returns true if the alarm was armed or waiting in its alarm queue
	bool OSCancelAlarm(OSAlarm_t* alarm)
	{
		__OSLockScheduler();
		bool cancelled = false;
		auto it = s_armed.find(alarm);
		if (it != s_armed.end())
		{
			Disarm(it);
			cancelled = true;
		}
		else if (WaitsInQueue(alarm))
		{
			QueueRemove(alarm);
			cancelled = true;
		}
		__OSUnlockScheduler();
		return cancelled;
	}

	void OSCancelAlarms(uint32 group)
	{
		if (group == 0 || group == GROUP_INVALID)
			return;
		__OSLockScheduler();
		for (auto it = s_armed.begin(); it != s_armed.end();)
		{
			auto current = it++;
			if (current->first->group == group)
				Disarm(current);
		}
		__OSUnlockScheduler();
	}

	void OSSetAlarmTag(OSAlarm_t* alarm, uint32 group)
	{
		if (group == GROUP_INVALID)
			return;
		__OSLockScheduler();
		alarm->group = group;
		__OSUnlockScheduler();
	}

	bool OSSetAlarm(OSAlarm_t* alarm, uint64 delayInTicks, MPTR handlerFunc)
	{
		if ((sint64)delayInTicks <= 0)
		{
			cemuLog_log(LogType::APIErrors, "OSSetAlarm(): the delay must be positive, alarm not set");
			return true;
		}
		__OSLockScheduler();
		Arm(alarm, OSGetTime() + delayInTicks, 0, handlerFunc, false, nullptr);
		__OSUnlockScheduler();
		return true;
	}

	bool OSSetPeriodicAlarm(OSAlarm_t* alarm, uint64 nextFire, uint64 period, MPTR handlerFunc)
	{
		__OSLockScheduler();
		Arm(alarm, nextFire, period, handlerFunc, true, nullptr);
		__OSUnlockScheduler();
		return true;
	}

	bool OSSetAlarmEx(OSAlarm_t* alarm, uint64 delayInTicks, MPTR handlerFunc, OSAlarmQueue* alarmQueue)
	{
		if ((sint64)delayInTicks <= 0)
		{
			cemuLog_log(LogType::APIErrors, "OSSetAlarmEx(): the delay must be positive, alarm not set");
			return true;
		}
		__OSLockScheduler();
		Arm(alarm, OSGetTime() + delayInTicks, 0, handlerFunc, false, alarmQueue);
		__OSUnlockScheduler();
		return true;
	}

	bool OSSetPeriodicAlarmEx(OSAlarm_t* alarm, uint64 nextFire, uint64 period, MPTR handlerFunc, OSAlarmQueue* alarmQueue)
	{
		__OSLockScheduler();
		Arm(alarm, nextFire, period, handlerFunc, true, alarmQueue);
		__OSUnlockScheduler();
		return true;
	}

	// takes the oldest fired alarm out of the queue (waiting for one unless noWait is set)
	bool OSGetAlarmFromQueue(OSAlarmQueue* queue, MEMPTR<OSAlarm_t>* alarmOut, MEMPTR<void>* contextOut, bool noWait)
	{
		__OSLockScheduler();
		while (!queue->head)
		{
			if (noWait)
			{
				*alarmOut = nullptr;
				*contextOut = nullptr;
				__OSUnlockScheduler();
				return false;
			}
			queue->threadQueue.queueAndWait(OSGetCurrentThread());
		}
		OSAlarm_t* alarm = queue->head.GetPtr();
		QueueRemove(alarm);
		// a periodic alarm continues once it has left the queue
		if (alarm->period != 0)
			Arm(alarm, alarm->startTime, alarm->period, alarm->handler, true, queue);
		*alarmOut = alarm;
		*contextOut = alarm->context;
		__OSUnlockScheduler();
		return true;
	}

	void OSSetAlarmUserData(OSAlarm_t* alarm, uint32 userData)
	{
		alarm->userData = userData;
	}

	uint32 OSGetAlarmUserData(OSAlarm_t* alarm)
	{
		return alarm->userData;
	}

	void OSAlarm_Shutdown()
	{
		__OSLockScheduler();
		if (!s_armed.empty())
		{
			for (auto& [alarm, host] : s_armed)
				OSHostAlarmDestroy(host);
			s_armed.clear();
			OSHostAlarm::ForgetAll();
		}
		__OSUnlockScheduler();
	}

	// guest thread that runs alarm handlers
	static void AlarmThreadMain(PPCInterpreter_t* hCPU)
	{
		OSContext_t* threadContext = &g_alarmThread.GetPtr()->context;
		while (true)
		{
			OSWaitEvent(g_alarmEvent.GetPtr());
			const uint64 now = OSGetTime();
			while (true)
			{
				__OSLockScheduler();
				OSAlarm_t* alarm = EarliestDue(now);
				if (!alarm)
				{
					__OSUnlockScheduler();
					break;
				}
				if (alarm->alarmQueue)
				{
					// queued alarms are handed to the queue instead of running a handler
					Disarm(s_armed.find(alarm));
					alarm->context = threadContext;
					QueuePush(alarm);
					__OSUnlockScheduler();
					continue;
				}
				if (alarm->period == 0)
					Disarm(s_armed.find(alarm)); // one-shot: done
				else
					alarm->nextTime = alarm->nextTime + alarm->period;
				__OSUnlockScheduler();
				PPCCoreCallback(alarm->handler, alarm, threadContext);
			}
		}
	}

	void MapAlarmExports()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::CoreinitAlarm)
		EXPORT_FN(OSCreateAlarm); EXPORT_FN(OSCreateAlarmEx); EXPORT_FN(OSCancelAlarm);
		EXPORT_FN(OSCancelAlarms); EXPORT_FN(OSSetAlarmTag); EXPORT_FN(OSSetAlarm);
		EXPORT_FN(OSSetPeriodicAlarm); EXPORT_FN(OSSetAlarmEx); EXPORT_FN(OSSetPeriodicAlarmEx);
		EXPORT_FN(OSInitAlarmQueue); EXPORT_FN(OSInitAlarmQueueEx); EXPORT_FN(OSGetAlarmFromQueue);
		EXPORT_FN(OSSetAlarmUserData); EXPORT_FN(OSGetAlarmUserData);
#undef EXPORT_FN
	}

	void InitializeAlarm()
	{
		OSInitEvent(g_alarmEvent.GetPtr(), OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, OSEvent::EVENT_MODE::MODE_AUTO);
		OSCreateThreadType(g_alarmThread.GetPtr(), RPLLoader_MakePPCCallable(AlarmThreadMain), 0, nullptr,
			g_alarmThreadStack.GetPtr() + g_alarmThreadStack.GetByteSize(), (sint32)g_alarmThreadStack.GetByteSize(), 0, 0x7, OSThread_t::THREAD_TYPE::TYPE_IO);
		OSResumeThread(g_alarmThread.GetPtr());
		strcpy(g_alarmThreadName.GetPtr(), "Alarm Thread");
		OSSetThreadName(g_alarmThread.GetPtr(), g_alarmThreadName.GetPtr());
	}
}
