// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <atomic>

// Small non-recursive spinlock (BasicLockable/Lockable). Spins on a plain
// load while the lock is held to avoid hammering the cache line.
class FSpinlock
{
public:
	bool is_locked() const
	{
		return m_held.load(std::memory_order_relaxed);
	}

	void lock() const
	{
		while (m_held.exchange(true, std::memory_order_acquire))
		{
			do
			{
				_mm_pause();
			} while (m_held.load(std::memory_order_relaxed));
		}
	}

	bool try_lock() const
	{
		return !m_held.exchange(true, std::memory_order_acquire);
	}

	void unlock() const
	{
		m_held.store(false, std::memory_order_release);
	}

private:
	mutable std::atomic<bool> m_held{ false };
};
