// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Single-consumer queue that hands items from host threads to one guest
// thread. The guest thread suspends itself while the queue is empty and every
// push resumes it once, so suspend and resume counts stay balanced. All
// queue accesses happen under the guest scheduler lock.
#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>

#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"

template <typename T>
class PPCConcurrentQueue
{
public:
	PPCConcurrentQueue() = default;
	PPCConcurrentQueue(const PPCConcurrentQueue&) = delete;
	PPCConcurrentQueue& operator=(const PPCConcurrentQueue&) = delete;

	// add an item and wake the consumer thread
	void push(const T& item, OSThread_t* consumer)
	{
		__OSLockScheduler();
		m_items.push(item);
		coreinit::__OSResumeThreadInternal(consumer, 1);
		__OSUnlockScheduler();
	}

	// called on the consumer thread itself; blocks while nothing is queued
	T pop(OSThread_t* consumer = nullptr)
	{
		cemu_assert_debug(consumer == nullptr || consumer == coreinit::OSGetCurrentThread());
		__OSLockScheduler();
		if (m_items.empty())
			coreinit::__OSSuspendThreadInternal(consumer);
		T front = m_items.front();
		m_items.pop();
		__OSUnlockScheduler();
		return front;
	}

private:
	std::queue<T> m_items;
};
