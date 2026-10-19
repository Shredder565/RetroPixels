// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Older request path used by some services: the PPC caller queues an entry
// for a device and is suspended until the device thread completes it.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "iosu_ioctl.h"
#include "util/helpers/Semaphore.h"

namespace
{
	struct DeviceQueue
	{
		std::mutex lock;
		std::deque<ioQueueEntry_t*> pending;
		CounterSemaphore available;

		ioQueueEntry_t* PopFront()
		{
			std::lock_guard l(lock);
			if (pending.empty())
				return nullptr;
			ioQueueEntry_t* e = pending.front();
			pending.pop_front();
			return e;
		}
	};

	DeviceQueue g_devices[IOS_DEVICE_COUNT];

	bool IsQueuedDevice(uint32 device)
	{
		switch (device)
		{
		case IOS_DEVICE_ACT:
		case IOS_DEVICE_ACP_MAIN:
		case IOS_DEVICE_MCP:
		case IOS_DEVICE_BOSS:
		case IOS_DEVICE_NIM:
		case IOS_DEVICE_FPD:
			return true;
		default:
			return false;
		}
	}
}

sint32 iosuIoctl_pushAndWait(uint32 ioctlHandle, ioQueueEntry_t* ioQueueEntry)
{
	if (!IsQueuedDevice(ioctlHandle))
	{
		cemuLog_logDebug(LogType::Force, "Unsupported IOSU device {}", ioctlHandle);
		cemu_assert_debug(false);
		return 0;
	}
	DeviceQueue& q = g_devices[ioctlHandle];
	__OSLockScheduler();
	{
		std::lock_guard l(q.lock);
		ioQueueEntry->ppcThread = coreinit::OSGetCurrentThread();
		q.pending.push_back(ioQueueEntry);
	}
	q.available.increment();
	// sleeps until iosuIoctl_completeRequest resumes the thread
	coreinit::__OSSuspendThreadInternal(coreinit::OSGetCurrentThread());
	cemu_assert(ioQueueEntry->isCompleted);
	__OSUnlockScheduler();
	return ioQueueEntry->returnValue;
}

ioQueueEntry_t* iosuIoctl_getNextWithWait(uint32 deviceIndex)
{
	DeviceQueue& q = g_devices[deviceIndex];
	q.available.decrementWithWait();
	ioQueueEntry_t* e = q.PopFront();
	cemu_assert(e != nullptr);
	return e;
}

ioQueueEntry_t* iosuIoctl_getNextWithTimeout(uint32 deviceIndex, sint32 ms)
{
	DeviceQueue& q = g_devices[deviceIndex];
	if (!q.available.decrementWithWaitAndTimeout(ms))
		return nullptr;
	return q.PopFront();
}

void iosuIoctl_completeRequest(ioQueueEntry_t* ioQueueEntry, uint32 returnValue)
{
	ioQueueEntry->returnValue = returnValue;
	ioQueueEntry->isCompleted = true;
	coreinit::OSResumeThread(ioQueueEntry->ppcThread);
}

void iosuIoctl_init()
{
	for (DeviceQueue& q : g_devices)
	{
		std::lock_guard l(q.lock);
		q.pending.clear();
	}
}
