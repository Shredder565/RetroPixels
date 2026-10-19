// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"

namespace coreinit
{
	// coreinit_Misc.cpp: feeds pending system messages into the queue / reacts to received ones
	void UpdateSystemMessageQueue();
	void HandleReceivedSystemMessage(OSMessage* msg);

	SysAllocator<OSMessageQueue> s_systemMessageQueue;
	SysAllocator<OSMessage, 16> s_systemMessageStorage;

	namespace
	{
		constexpr uint32 kMessageQueueMagic = 0x6D536751; // 'mSgQ'

		bool IsEmpty(const OSMessageQueue* q)
		{
			return (uint32)q->usedCount == 0;
		}

		bool IsFull(const OSMessageQueue* q)
		{
			return (uint32)q->usedCount >= (uint32)q->msgCount;
		}

		OSMessage* SlotAt(OSMessageQueue* q, uint32 index)
		{
			return q->msgArray.GetPtr() + index;
		}
	}

	void OSInitMessageQueueEx(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount, void* userData)
	{
		msgQueue->magic = kMessageQueueMagic;
		msgQueue->userData = userData;
		msgQueue->ukn08 = 0;
		msgQueue->msgArray = msgArray;
		msgQueue->msgCount = msgCount;
		msgQueue->firstIndex = 0;
		msgQueue->usedCount = 0;
		OSInitThreadQueueEx(&msgQueue->threadQueueReceive, msgQueue);
		OSInitThreadQueueEx(&msgQueue->threadQueueSend, msgQueue);
	}

	void OSInitMessageQueue(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount)
	{
		OSInitMessageQueueEx(msgQueue, msgArray, msgCount, nullptr);
	}

	bool OSReceiveMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		const bool fromSystemQueue = msgQueue == s_systemMessageQueue.GetPtr();
		if (fromSystemQueue)
			UpdateSystemMessageQueue();
		__OSLockScheduler(msgQueue);
		while (IsEmpty(msgQueue))
		{
			if ((flags & OS_MESSAGE_BLOCK) == 0)
			{
				__OSUnlockScheduler(msgQueue);
				return false;
			}
			msgQueue->threadQueueReceive.queueAndWait(OSGetCurrentThread());
		}
		uint32 first = msgQueue->firstIndex;
		*msg = *SlotAt(msgQueue, first);
		msgQueue->firstIndex = (first + 1) % (uint32)msgQueue->msgCount;
		msgQueue->usedCount = (uint32)msgQueue->usedCount - 1;
		// one slot became free
		if (!msgQueue->threadQueueSend.isEmpty())
			msgQueue->threadQueueSend.wakeupSingleThreadWaitQueue(true);
		__OSUnlockScheduler(msgQueue);
		if (fromSystemQueue)
			HandleReceivedSystemMessage(msg);
		return true;
	}

	// copies the oldest message without removing it; msg may be null to only test for one
	bool OSPeekMessage(OSMessageQueue* msgQueue, OSMessage* msg)
	{
		__OSLockScheduler(msgQueue);
		bool available = !IsEmpty(msgQueue);
		if (available && msg)
			*msg = *SlotAt(msgQueue, msgQueue->firstIndex);
		__OSUnlockScheduler(msgQueue);
		return available;
	}

	sint32 OSSendMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		__OSLockScheduler();
		while (IsFull(msgQueue))
		{
			if ((flags & OS_MESSAGE_BLOCK) == 0)
			{
				__OSUnlockScheduler();
				return 0;
			}
			msgQueue->threadQueueSend.queueAndWait(OSGetCurrentThread());
		}
		uint32 slot;
		if ((flags & OS_MESSAGE_HIGH_PRIORITY) != 0)
		{
			// step the ring start back by one and store the message there
			sint32 capacity = (sint32)(uint32)msgQueue->msgCount;
			slot = (uint32)(((sint32)(uint32)msgQueue->firstIndex + capacity - 1) % capacity);
			msgQueue->firstIndex = slot;
		}
		else
		{
			slot = ((uint32)msgQueue->firstIndex + (uint32)msgQueue->usedCount) % (uint32)msgQueue->msgCount;
		}
		msgQueue->usedCount = (uint32)msgQueue->usedCount + 1;
		*SlotAt(msgQueue, slot) = *msg;
		if (!msgQueue->threadQueueReceive.isEmpty())
			msgQueue->threadQueueReceive.wakeupSingleThreadWaitQueue(true);
		__OSUnlockScheduler();
		return 1;
	}

	OSMessageQueue* OSGetSystemMessageQueue()
	{
		return s_systemMessageQueue.GetPtr();
	}

	void InitializeMessageQueue()
	{
		OSInitMessageQueue(s_systemMessageQueue.GetPtr(), s_systemMessageStorage.GetPtr(), s_systemMessageStorage.GetCount());

		cafeExportRegister("coreinit", OSInitMessageQueueEx, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSInitMessageQueue, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSReceiveMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSPeekMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSSendMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetSystemMessageQueue, LogType::CoreinitThread);
	}
};
