// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <mutex>
#include <condition_variable>

// Counting semaphore: notify() adds a permit, wait() takes one (blocking).
class Semaphore
{
public:
	void notify()
	{
		std::lock_guard lock(m_lock);
		m_permits++;
		m_changed.notify_one();
	}

	void wait()
	{
		std::unique_lock lock(m_lock);
		m_changed.wait(lock, [this] { return m_permits != 0; });
		m_permits--;
	}

	bool try_wait()
	{
		std::lock_guard lock(m_lock);
		if (m_permits == 0)
			return false;
		m_permits--;
		return true;
	}

	void reset()
	{
		std::lock_guard lock(m_lock);
		m_permits = 0;
	}

private:
	std::mutex m_lock;
	std::condition_variable m_changed;
	uint64 m_permits = 0;
};

// Counter that other threads can wait on until it becomes zero or non-zero.
class CounterSemaphore
{
public:
	void reset()
	{
		std::lock_guard lock(m_lock);
		m_value = 0;
	}

	void increment()
	{
		std::lock_guard lock(m_lock);
		if (++m_value == 1)
			m_changed.notify_all(); // left zero
	}

	void decrement()
	{
		std::lock_guard lock(m_lock);
		--m_value;
		cemu_assert_debug(m_value >= 0);
		if (m_value == 0)
			m_changed.notify_all();
	}

	// waits until the counter is non-zero, then decrements it
	void decrementWithWait()
	{
		std::unique_lock lock(m_lock);
		m_changed.wait(lock, [this] { return m_value != 0; });
		--m_value;
	}

	// like decrementWithWait() but gives up after waiting once for up to ms milliseconds
	// (the wait may also end early); returns whether it decremented
	bool decrementWithWaitAndTimeout(uint32 ms)
	{
		std::unique_lock lock(m_lock);
		if (m_value == 0)
			m_changed.wait_for(lock, std::chrono::milliseconds(ms));
		if (m_value == 0)
			return false;
		--m_value;
		return true;
	}

	void waitUntilZero()
	{
		std::unique_lock lock(m_lock);
		m_changed.wait(lock, [this] { return m_value == 0; });
	}

	void waitUntilNonZero()
	{
		std::unique_lock lock(m_lock);
		m_changed.wait(lock, [this] { return m_value != 0; });
	}

	// unsynchronized snapshot
	bool isZero() const
	{
		return m_value == 0;
	}

private:
	std::mutex m_lock;
	std::condition_variable m_changed;
	sint64 m_value = 0;
};

// A value that threads can wait on until it reaches a given state.
template<typename T>
class StateSemaphore
{
public:
	StateSemaphore(T initialState) : m_state(initialState) {}

	T getValue()
	{
		std::lock_guard lock(m_lock);
		return m_state;
	}

	bool hasState(T state)
	{
		std::lock_guard lock(m_lock);
		return m_state == state;
	}

	void setValue(T newState)
	{
		std::lock_guard lock(m_lock);
		m_state = newState;
		m_changed.notify_all();
	}

	// waits until the value is expectedValue, then replaces it with newState
	void setValue(T newState, T expectedValue)
	{
		std::unique_lock lock(m_lock);
		m_changed.wait(lock, [&] { return m_state == expectedValue; });
		m_state = newState;
		m_changed.notify_all();
	}

	void waitUntilValue(T state)
	{
		std::unique_lock lock(m_lock);
		m_changed.wait(lock, [&] { return m_state == state; });
	}

private:
	std::mutex m_lock;
	std::condition_variable m_changed;
	T m_state;
};
