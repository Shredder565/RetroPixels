// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MPQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "util/helpers/fspinlock.h"

// Used by Yoshi's Woolly World, Fast Racing Neo, Tokyo Mirage Sessions and Mii Maker.
//
// The console guards each queue with the spinlock inside MPTaskQ. Titles reinitialise a queue
// (MPTermTaskQ + MPInitTaskQ, which resets that spinlock) while their worker threads may still be
// in MPDequeTask/MPRunTask on it, which corrupts the lock. One global host lock is used for all
// queues instead. All per core counters live in index 1 of the arrays.

namespace coreinit
{
	FSpinlock s_taskQueueLock;

	namespace
	{
		constexpr uint32 kSharedSlot = 1;

		struct TaskQueueGuard
		{
			TaskQueueGuard() { s_taskQueueLock.lock(); }
			~TaskQueueGuard() { s_taskQueueLock.unlock(); }
		};

		bool IsStopState(uint32 state)
		{
			return state == MP_TASKQ_STATE_STOPPING || state == MP_TASKQ_STATE_STOP;
		}

		// runs the task function on the calling thread and records core and duration
		void ExecuteTask(MPTask* task)
		{
			uint64 begin = OSGetSystemTime();
			task->taskState = MP_TASK_STATE_RUN;
			task->coreIndex = OSGetCoreId();
			task->taskFunc.result = PPCCoreCallback(task->taskFunc.func, task->taskFunc.data, task->taskFunc.size);
			task->taskState = MP_TASK_STATE_DONE;
			task->runtime = (sint64)(OSGetSystemTime() - begin);
		}

		// with the lock held: account for finished tasks and move the queue to STOP/DONE
		void RecordFinished(MPTaskQ* taskq, uint32 finishedCount)
		{
			uint32 stillRunning = (uint32)taskq->taskRunCount - finishedCount;
			taskq->taskRunCount = stillRunning;
			uint32 done = (uint32)taskq->taskDoneCount[kSharedSlot] + finishedCount;
			taskq->taskDoneCount[kSharedSlot] = done;
			if (taskq->state == MP_TASKQ_STATE_STOPPING && stillRunning == 0)
				taskq->state = MP_TASKQ_STATE_STOP;
			if (taskq->taskCount == done)
				taskq->state = MP_TASKQ_STATE_DONE;
		}

		// waitState is a mask of RUN/STOP/DONE. Reaching RUN or STOP also ends a wait for DONE, and
		// DONE ends all of them. Returns false if giveUp() said so before the wait ended.
		template<typename TIdle>
		bool WaitForQueueState(MPTaskQ* taskq, uint32 waitState, TIdle idle)
		{
			bool wantRun = HAS_FLAG(waitState, MP_TASKQ_STATE_RUN);
			bool wantStop = HAS_FLAG(waitState, MP_TASKQ_STATE_STOP);
			bool wantDone = HAS_FLAG(waitState, MP_TASKQ_STATE_DONE);
			while (wantRun || wantStop || wantDone)
			{
				uint32 state = taskq->state;
				if (wantRun && HAS_FLAG(state, MP_TASKQ_STATE_RUN))
				{
					wantRun = wantDone = false;
					continue;
				}
				if (wantStop && HAS_FLAG(state, MP_TASKQ_STATE_STOP))
				{
					wantStop = wantDone = false;
					continue;
				}
				if (wantDone && HAS_FLAG(state, MP_TASKQ_STATE_DONE))
				{
					wantRun = wantStop = wantDone = false;
					continue;
				}
				if (!idle())
					return false;
			}
			return true;
		}
	}

	/* tasks */

	void MPInitTask(MPTask* task, void* func, void* data, uint32 size)
	{
		TaskQueueGuard guard;
		task->thisptr = task;
		task->taskState = MP_TASK_STATE_INIT;
		task->taskFunc.func = func;
		task->taskFunc.data = data;
		task->taskFunc.size = size;
		task->coreIndex = PPC_CORE_COUNT;
		task->runtime = 0;
		task->userdata = nullptr;
	}

	bool MPTermTask(MPTask* task)
	{
		return true;
	}

	bool MPRunTask(MPTask* task)
	{
		if (task->taskState != MP_TASK_STATE_READY)
			return false;
		MPTaskQ* taskq = task->taskQ.GetPtr();
		if (IsStopState(taskq->state))
			return false;
		{
			TaskQueueGuard guard;
			if (IsStopState(taskq->state))
				return false;
			taskq->taskReadyCount = taskq->taskReadyCount - 1;
			taskq->taskRunCount = taskq->taskRunCount + 1;
		}
		ExecuteTask(task);
		cemu_assert_debug(taskq->state != MP_TASKQ_STATE_DONE);
		TaskQueueGuard guard;
		RecordFinished(taskq, 1);
		return true;
	}

	bool MPGetTaskInfo(MPTask* task, MPTaskInfo* info)
	{
		// funcResult is left untouched
		info->state = task->taskState;
		info->coreIndex = task->coreIndex;
		info->runtime = task->runtime;
		return true;
	}

	void* MPGetTaskUserData(MPTask* task)
	{
		return task->userdata.GetPtr();
	}

	void MPSetTaskUserData(MPTask* task, void* userdata)
	{
		task->userdata = userdata;
	}

	/* task queues */

	void MPInitTaskQ(MPTaskQ* taskq, MPTask** tasks, uint32 taskCount)
	{
		TaskQueueGuard guard;
		taskq->thisptr = taskq;
		OSInitSpinLock(&taskq->spinlock);
		taskq->state = MP_TASKQ_STATE_INIT;
		taskq->taskCount = 0;
		taskq->taskReadyCount = 0;
		taskq->taskRunCount = 0;
		for (uint32 core = 0; core < OSGetCoreCount(); core++)
		{
			taskq->taskDoneCount[core] = 0;
			taskq->nextIndex[core] = 0;
			taskq->endIndex[core] = 0;
		}
		taskq->taskQueue = (MEMPTR<MPTask>*)tasks;
		taskq->taskQueueSize = taskCount;
	}

	bool MPEnqueTask(MPTaskQ* taskq, MPTask* task)
	{
		if (task->taskState != MP_TASK_STATE_INIT)
			return false;
		TaskQueueGuard guard;
		uint32 state = taskq->state;
		constexpr uint32 kAcceptingStates = MP_TASKQ_STATE_INIT | MP_TASKQ_STATE_RUN | MP_TASKQ_STATE_STOPPING | MP_TASKQ_STATE_STOP | MP_TASKQ_STATE_DONE;
		// the state is a single bit; anything else is rejected
		bool validState = state != 0 && (state & (state - 1)) == 0 && (state & kAcceptingStates) == state;
		uint32 slot = (uint32)(sint32)taskq->endIndex[kSharedSlot];
		if (slot >= (uint32)taskq->taskQueueSize || !validState)
			return false;
		task->taskQ = taskq;
		task->taskState = MP_TASK_STATE_READY;
		taskq->thisptr = taskq;
		taskq->endIndex[kSharedSlot] = (sint32)(slot + 1);
		taskq->taskCount = taskq->taskCount + 1;
		taskq->taskReadyCount = taskq->taskReadyCount + 1;
		taskq->taskQueue[slot] = task;
		// adding work to a finished queue makes it run again
		if (state == MP_TASKQ_STATE_DONE)
			taskq->state = MP_TASKQ_STATE_RUN;
		return true;
	}

	bool MPTermTaskQ(MPTaskQ* taskq)
	{
		s_taskQueueLock.lock();
		// stops a running queue and waits until no task runs anymore (needed by Tokyo Mirage Sessions)
		if (taskq->state == MP_TASKQ_STATE_RUN)
			taskq->state = MP_TASKQ_STATE_STOP;
		while (taskq->taskRunCount != 0)
		{
			s_taskQueueLock.unlock();
			OSYieldThread();
			s_taskQueueLock.lock();
		}
		s_taskQueueLock.unlock();
		return true;
	}

	bool MPGetTaskQInfo(MPTaskQ* taskq, MPTaskQInfo* info)
	{
		TaskQueueGuard guard;
		info->state = taskq->state;
		info->taskCount = taskq->taskCount;
		info->taskReadyCount = taskq->taskReadyCount;
		info->taskRunCount = taskq->taskRunCount;
		info->taskDoneCount = taskq->taskDoneCount[kSharedSlot];
		return true;
	}

	bool MPStartTaskQ(MPTaskQ* taskq)
	{
		TaskQueueGuard guard;
		if (taskq->state != MP_TASKQ_STATE_INIT && taskq->state != MP_TASKQ_STATE_STOP)
			return false;
		taskq->state = MP_TASKQ_STATE_RUN;
		return true;
	}

	// Takes up to granularity tasks at a time and runs them on the calling thread until the queue
	// is empty or leaves the RUN state. Returns true if it ran anything (or the queue is done).
	bool MPRunTasksFromTaskQ(MPTaskQ* taskq, int granularity)
	{
		uint32 lastTask = MPTR_NULL;
		while (true)
		{
			uint32 state = taskq->state;
			if (state != MP_TASKQ_STATE_RUN)
				return (state & MP_TASKQ_STATE_DONE) != 0;
			sint32 first;
			sint32 end;
			sint32 count;
			{
				TaskQueueGuard guard;
				first = taskq->nextIndex[kSharedSlot];
				sint32 available = taskq->endIndex[kSharedSlot];
				if (first == available)
					break;
				end = first + granularity;
				if (available < end)
					end = available;
				count = end - first;
				taskq->nextIndex[kSharedSlot] = end;
				taskq->taskReadyCount = (uint32)taskq->taskReadyCount - (uint32)count;
				taskq->taskRunCount = (uint32)taskq->taskRunCount + (uint32)count;
			}
			// give other threads a chance between batches
			if (lastTask != MPTR_NULL)
				PPCCore_switchToScheduler();
			for (sint32 i = first; i < end; i++)
			{
				MPTask* task = taskq->taskQueue[i].GetPtr();
				lastTask = task->thisptr.GetMPTR();
				ExecuteTask(task);
			}
			TaskQueueGuard guard;
			RecordFinished(taskq, (uint32)count);
		}
		return lastTask != MPTR_NULL;
	}

	bool MPStopTaskQ(MPTaskQ* taskq)
	{
		TaskQueueGuard guard;
		if (taskq->state != MP_TASKQ_STATE_RUN)
			return false;
		taskq->state = taskq->taskRunCount == 0 ? MP_TASKQ_STATE_STOP : MP_TASKQ_STATE_STOPPING;
		return true;
	}

	bool MPWaitTaskQ(MPTaskQ* taskq, uint32 waitState)
	{
		bool firstPass = true;
		return WaitForQueueState(taskq, waitState, [&firstPass]() {
			// after the first plain yield, sleep 0.05ms per round so lower priority workers get to
			// run (Yoshi's Woolly World soft locks otherwise)
			if (firstPass)
				PPCCore_switchToScheduler();
			else
				OSSleepTicks(EspressoTime::ConvertNsToTimerTicks(50000));
			firstPass = false;
			return true;
		});
	}

	bool MPWaitTaskQWithTimeout(MPTaskQ* taskq, uint32 waitState, sint64 timeout)
	{
		uint64 deadline = OSGetSystemTime() + EspressoTime::ConvertNsToTimerTicks(timeout);
		return WaitForQueueState(taskq, waitState, [&]() {
			if (OSGetSystemTime() >= deadline)
			{
				if (waitState == MP_TASKQ_STATE_DONE)
					cemuLog_log(LogType::Force, "MPWaitTaskQWithTimeout: timed out waiting for the queue to finish");
				return false;
			}
			PPCCore_switchToScheduler();
			return true;
		});
	}

	MPTask* MPDequeTask(MPTaskQ* taskq)
	{
		if (taskq->state != MP_TASKQ_STATE_RUN)
			return nullptr;
		TaskQueueGuard guard;
		sint32 next = taskq->nextIndex[kSharedSlot];
		if (taskq->state != MP_TASKQ_STATE_RUN || next == (sint32)taskq->endIndex[kSharedSlot])
			return nullptr;
		taskq->nextIndex[kSharedSlot] = next + 1;
		return taskq->taskQueue[next].GetPtr();
	}

	uint32 MPDequeTasks(MPTaskQ* taskq, MPTask** tasks, sint32 maxTasks)
	{
		if (taskq->state != MP_TASKQ_STATE_RUN)
			return 0;
		TaskQueueGuard guard;
		if (taskq->state != MP_TASKQ_STATE_RUN)
			return 0;
		sint32 first = taskq->nextIndex[kSharedSlot];
		sint32 end = first + maxTasks;
		if ((sint32)taskq->endIndex[kSharedSlot] < end)
			end = taskq->endIndex[kSharedSlot];
		taskq->nextIndex[kSharedSlot] = end;
		for (sint32 i = first; i < end; i++)
			tasks[i - first] = taskq->taskQueue[i].GetPtr();
		return (uint32)(end - first);
	}

	// puts a stopped or finished queue back to INIT with all its tasks ready to run again
	bool MPResetTaskQ(MPTaskQ* taskq)
	{
		debug_printf("MPResetTaskQ called\n");
		TaskQueueGuard guard;
		if (taskq->state != MP_TASKQ_STATE_DONE && taskq->state != MP_TASKQ_STATE_STOP)
			return false;
		taskq->state = MP_TASKQ_STATE_INIT;
		taskq->taskRunCount = 0;
		uint32 queued = (uint32)(sint32)taskq->endIndex[kSharedSlot];
		taskq->taskCount = queued;
		taskq->taskReadyCount = queued;
		for (uint32 core = 0; core < OSGetCoreCount(); core++)
		{
			taskq->taskDoneCount[core] = 0;
			taskq->nextIndex[core] = 0;
		}
		for (uint32 i = 0; i < queued; i++)
		{
			MPTask* task = taskq->taskQueue[i].GetPtr();
			task->taskState = MP_TASK_STATE_READY;
			task->taskFunc.result = 0;
			task->coreIndex = PPC_CORE_COUNT;
			task->runtime = 0;
		}
		return true;
	}

	void InitializeMP()
	{
		cafeExportRegister("coreinit", MPInitTask, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPTermTask, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPRunTask, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPGetTaskInfo, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPGetTaskUserData, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPSetTaskUserData, LogType::CoreinitMP);

		cafeExportRegister("coreinit", MPInitTaskQ, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPResetTaskQ, LogType::CoreinitMP);

		cafeExportRegister("coreinit", MPEnqueTask, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPDequeTask, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPDequeTasks, LogType::CoreinitMP);

		cafeExportRegister("coreinit", MPRunTasksFromTaskQ, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPStartTaskQ, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPWaitTaskQ, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPWaitTaskQWithTimeout, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPStopTaskQ, LogType::CoreinitMP);

		cafeExportRegister("coreinit", MPTermTaskQ, LogType::CoreinitMP);
		cafeExportRegister("coreinit", MPGetTaskQInfo, LogType::CoreinitMP);
	}
}
