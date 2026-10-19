// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"

namespace coreinit
{
	// messages the system sends to the application's system message queue
	enum class SysMessageId : uint32
	{
		MsgAcquireForeground = 0xFACEF000,
		MsgReleaseForeground = 0xFACEBACC,
		MsgExit = 0xD1E0D1E0,
		HomeButtonDenied = 0xCCC0FFEE,
		NetIoStartOrStop = 0xAAC0FFEE,
	};

	struct OSMessage
	{
		uint32be message;
		uint32be data0;
		uint32be data1;
		uint32be data2;
	};
	static_assert(sizeof(OSMessage) == 0x10);

	// ring buffer of fixed size messages with queues for blocked senders and receivers
	struct OSMessageQueue
	{
		/* +0x00 */ uint32be magic; // 'mSgQ'
		/* +0x04 */ MEMPTR<void> userData;
		/* +0x08 */ uint32be ukn08;
		/* +0x0C */ OSThreadQueue threadQueueSend;
		/* +0x1C */ OSThreadQueue threadQueueReceive;
		/* +0x2C */ MEMPTR<OSMessage> msgArray;
		/* +0x30 */ uint32be msgCount;   // ring capacity
		/* +0x34 */ uint32be firstIndex; // oldest message
		/* +0x38 */ uint32be usedCount;
	};
	static_assert(sizeof(OSMessageQueue) == 0x3C);

	// OSSendMessage/OSReceiveMessage flags
	#define OS_MESSAGE_BLOCK				1 // wait for space or for a message
	#define OS_MESSAGE_HIGH_PRIORITY		2 // send: put the message in front of the queued ones

	void OSInitMessageQueueEx(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount, void* userData);
	void OSInitMessageQueue(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount);
	bool OSReceiveMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags);
	bool OSPeekMessage(OSMessageQueue* msgQueue, OSMessage* msg);
	sint32 OSSendMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags);

	OSMessageQueue* OSGetSystemMessageQueue();

	void InitializeMessageQueue();
};
