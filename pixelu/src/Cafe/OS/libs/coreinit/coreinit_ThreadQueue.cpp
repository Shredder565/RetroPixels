// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"

// Thread queues are intrusive lists: each OSThread_t embeds several OSThreadLink fields and a
// queue operation names the link it uses. The same link field is used in every thread of a queue.

namespace coreinit
{
	namespace
	{
		// position of a link field inside OSThread_t
		size_t LinkFieldOffset(OSThread_t* thread, OSThreadLink* link)
		{
			size_t offset = (size_t)((uint8*)link - (uint8*)thread);
			cemu_assert_debug(offset < sizeof(OSThread_t));
			return offset;
		}

		OSThreadLink& LinkOf(OSThread_t* thread, size_t offset)
		{
			return *(OSThreadLink*)((uint8*)thread + offset);
		}

		// the woken thread is made runnable; reports whether the caller should give up its core for it
		bool MakeWokenThreadReady(OSThread_t* thread, bool reschedule, bool sharedPriorityAndAffinityWorkaround)
		{
			cemu_assert_debug(thread->state == OSThread_t::THREAD_STATE::STATE_WAITING);
			thread->state = OSThread_t::THREAD_STATE::STATE_READY;
			thread->currentWaitQueue = nullptr;
			__OSAddReadyThreadToRunQueue(thread);
			if (!reschedule || thread->suspendCounter != 0 || !PPCInterpreter_getCurrentInstance())
				return false;
			return __OSCoreShouldSwitchToThread(OSGetCurrentThread(), thread, sharedPriorityAndAffinityWorkaround);
		}
	}

	void OSThreadQueueInternal::addThread(OSThread_t* thread, OSThreadLink* threadLink)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		size_t offset = LinkFieldOffset(thread, threadLink);
		threadLink->next = nullptr;
		threadLink->prev = tail;
		if (tail)
			LinkOf(tail.GetPtr(), offset).next = thread;
		else
			head = thread;
		tail = thread;
	}

	void OSThreadQueueInternal::addThreadByPriority(OSThread_t* thread, OSThreadLink* threadLink)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		cemu_assert_debug(head.IsNull() == tail.IsNull());
		size_t offset = LinkFieldOffset(thread, threadLink);
		// find the last thread that does not have a lower priority (larger value) than the new one,
		// so threads of equal priority stay in arrival order
		OSThread_t* before = tail.GetPtr();
		while (before && before->effectivePriority > thread->effectivePriority)
			before = LinkOf(before, offset).prev.GetPtr();
		if (!before)
		{
			// goes to the front (also covers the empty queue)
			threadLink->prev = nullptr;
			threadLink->next = head;
			if (head)
				LinkOf(head.GetPtr(), offset).prev = thread;
			else
				tail = thread;
			head = thread;
			return;
		}
		OSThreadLink& beforeLink = LinkOf(before, offset);
		OSThread_t* after = beforeLink.next.GetPtr();
		threadLink->prev = before;
		threadLink->next = after;
		beforeLink.next = thread;
		if (after)
			LinkOf(after, offset).prev = thread;
		else
			tail = thread;
	}

	void OSThreadQueueInternal::removeThread(OSThread_t* thread, OSThreadLink* threadLink)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		size_t offset = LinkFieldOffset(thread, threadLink);
		OSThread_t* prev = threadLink->prev.GetPtr();
		OSThread_t* next = threadLink->next.GetPtr();
		if (prev)
			LinkOf(prev, offset).next = next;
		else
			head = next;
		if (next)
			LinkOf(next, offset).prev = prev;
		else
			tail = prev;
		threadLink->next = nullptr;
		threadLink->prev = nullptr;
	}

	void OSThreadQueueInternal::queueOnly(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		cemu_assert_debug(thread->waitQueueLink.next.IsNull() && thread->waitQueueLink.prev.IsNull());
		cemu_assert_debug(thread->state == OSThread_t::THREAD_STATE::STATE_RUNNING);
		thread->currentWaitQueue = this;
		addThreadByPriority(thread, &thread->waitQueueLink);
		thread->state = OSThread_t::THREAD_STATE::STATE_WAITING;
	}

	void OSThreadQueueInternal::queueAndWait(OSThread_t* thread)
	{
		queueOnly(thread);
		// returns once another thread woke us and the scheduler picked us again
		PPCCore_switchToSchedulerWithLock();
		cemu_assert_debug(thread->state == OSThread_t::THREAD_STATE::STATE_RUNNING);
	}

	void OSThreadQueueInternal::cancelWait(OSThread_t* thread)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		removeThread(thread, &thread->waitQueueLink);
		thread->state = OSThread_t::THREAD_STATE::STATE_READY;
		thread->currentWaitQueue = nullptr;
		__OSAddReadyThreadToRunQueue(thread);
	}

	void OSThreadQueueInternal::wakeupEntireWaitQueue(bool reschedule, bool sharedPriorityAndAffinityWorkaround)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		bool yieldCore = false;
		while (OSThread_t* thread = head.GetPtr())
		{
			removeThread(thread, &thread->waitQueueLink);
			if (MakeWokenThreadReady(thread, reschedule, sharedPriorityAndAffinityWorkaround))
				yieldCore = true;
		}
		if (yieldCore)
			PPCCore_switchToSchedulerWithLock();
	}

	void OSThreadQueueInternal::wakeupSingleThreadWaitQueue(bool reschedule, bool sharedPriorityAndAffinityWorkaround)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		OSThread_t* thread = head.GetPtr();
		cemu_assert_debug(thread);
		if (!thread)
			return;
		removeThread(thread, &thread->waitQueueLink);
		if (MakeWokenThreadReady(thread, reschedule, sharedPriorityAndAffinityWorkaround))
			PPCCore_switchToSchedulerWithLock();
	}

	void OSMutexQueue::addMutex(OSMutex* mutex)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		mutex->next = nullptr;
		mutex->prev = tail;
		if (tail)
			tail->next = mutex;
		else
			head = mutex;
		tail = mutex;
	}

	void OSMutexQueue::removeMutex(OSMutex* mutex)
	{
		cemu_assert_debug(__OSHasSchedulerLock());
		cemu_assert_debug(!head.IsNull() && !tail.IsNull());
		OSMutex* prev = mutex->prev.GetPtr();
		OSMutex* next = mutex->next.GetPtr();
		if (prev)
			prev->next = next;
		else
			head = next;
		if (next)
			next->prev = prev;
		else
			tail = prev;
		mutex->next = nullptr;
		mutex->prev = nullptr;
	}
}
