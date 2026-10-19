// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>

// FIFO shared between threads. pop() and peek() block until an item is there.
template<typename T>
class ConcurrentQueue
{
public:
	ConcurrentQueue() = default;
	ConcurrentQueue(const ConcurrentQueue&) = delete;
	ConcurrentQueue& operator=(const ConcurrentQueue&) = delete;

	// returns the number of queued items after the push
	size_t push(const T& item)
	{
		return emplaceAndNotify(item);
	}

	template<typename... TArgs>
	size_t push(TArgs&&... args)
	{
		return emplaceAndNotify(std::forward<TArgs>(args)...);
	}

	// waits for an item and returns a copy of it without removing it
	T peek()
	{
		std::unique_lock lock(m_lock);
		waitNotEmpty(lock);
		return m_items.front();
	}

	// non-blocking: takes the front item if there is one
	bool peek2(T& out)
	{
		std::lock_guard lock(m_lock);
		if (m_items.empty())
			return false;
		out = m_items.front();
		m_items.pop();
		return true;
	}

	T pop()
	{
		std::unique_lock lock(m_lock);
		waitNotEmpty(lock);
		T front = std::move(m_items.front());
		m_items.pop();
		return front;
	}

	void pop(T& out)
	{
		std::unique_lock lock(m_lock);
		waitNotEmpty(lock);
		out = std::move(m_items.front());
		m_items.pop();
	}

	void clear()
	{
		std::lock_guard lock(m_lock);
		std::queue<T> empty;
		m_items.swap(empty);
	}

	size_t size()
	{
		std::lock_guard lock(m_lock);
		return m_items.size();
	}

	bool empty()
	{
		std::lock_guard lock(m_lock);
		return m_items.empty();
	}

private:
	template<typename... TArgs>
	size_t emplaceAndNotify(TArgs&&... args)
	{
		size_t count;
		{
			std::lock_guard lock(m_lock);
			m_items.emplace(std::forward<TArgs>(args)...);
			count = m_items.size();
		}
		m_itemAdded.notify_one();
		return count;
	}

	void waitNotEmpty(std::unique_lock<std::mutex>& lock)
	{
		m_itemAdded.wait(lock, [this] { return !m_items.empty(); });
	}

	std::mutex m_lock;
	std::condition_variable m_itemAdded;
	std::queue<T> m_items;
};
