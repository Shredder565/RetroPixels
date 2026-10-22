// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"

namespace coreinit
{
	// Host alarms: emulator-side timers checked by the scheduler. The callback runs on whichever
	// host thread notices the alarm is due, with the scheduler lock held. All calls need the lock.
	class OSHostAlarm;
	OSHostAlarm* OSHostAlarmCreate(uint64 nextFire, uint64 period, void(*callbackFunc)(uint64 currentTick, void* context), void* context);
	void OSHostAlarmDestroy(OSHostAlarm* hostAlarm);

	struct OSAlarm_t;

	// alarm queue: alarms set with an alarm queue are appended to it when they fire instead of
	// running their handler; OSGetAlarmFromQueue takes them out
	struct OSAlarmQueue
	{
		/* +0x00 */ uint32be magic;
		/* +0x04 */ MEMPTR<const char> name;
		/* +0x08 */ uint32be ukn08;
		/* +0x0C */ OSThreadQueue threadQueue;
		/* +0x1C */ MEMPTR<OSAlarm_t> head;
		/* +0x20 */ MEMPTR<OSAlarm_t> tail;
	};
	static_assert(sizeof(OSAlarmQueue) == 0x24);

	struct OSAlarm_t
	{
		/* +0x00 */ betype<uint32> magic;
		/* +0x04 */ MEMPTR<const char> name;
		/* +0x08 */ uint32 ukn08;
		/* +0x0C */ uint32be handler;
		/* +0x10 */ uint32be group;
		/* +0x14 */ uint32 padding14;
		/* +0x18 */ uint64be nextTime;  // next fire time
		/* +0x20 */ MEMPTR<OSAlarm_t> prev; // links inside an alarm queue
		/* +0x24 */ MEMPTR<OSAlarm_t> next;
		/* +0x28 */ uint64be period;    // zero for one-shot alarms
		/* +0x30 */ uint64be startTime; // start of the period grid
		/* +0x38 */ uint32be userData;
		/* +0x3C */ uint32 ukn3C;
		/* +0x40 */ OSThreadQueue uknThreadQueue;
		/* +0x50 */ MEMPTR<OSAlarmQueue> alarmQueue;
		/* +0x54 */ MEMPTR<void> context;

		static constexpr uint32 MAGIC = 0x614C724D; // 'aLrM'

		void setMagic()
		{
			magic = MAGIC;
		}

		bool checkMagic()
		{
			return magic == MAGIC;
		}
	};
	static_assert(offsetof(OSAlarm_t, nextTime) == 0x18);
	static_assert(offsetof(OSAlarm_t, uknThreadQueue) == 0x40);
	static_assert(sizeof(OSAlarm_t) == 0x58);

	void OSCreateAlarm(OSAlarm_t* alarm);
	bool OSCancelAlarm(OSAlarm_t* alarm);
	bool OSSetAlarm(OSAlarm_t* alarm, uint64 time, MPTR handlerFunc);
	void OSSetAlarmUserData(OSAlarm_t* alarm, uint32 userData);
	bool OSSetPeriodicAlarm(OSAlarm_t* OSAlarm, uint64 startTick, uint64 periodTick, MPTR OSAlarmHandler);

	// cancels every alarm (called when the title shuts down)
	void OSAlarm_Shutdown();

	// called by the scheduler: fires host alarms that are due
	void alarm_update();

	void MapAlarmExports();
	void InitializeAlarm();
}
