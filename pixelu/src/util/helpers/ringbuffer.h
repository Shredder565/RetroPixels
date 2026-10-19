// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <mutex>

// Fixed-size ring buffer guarded by a mutex. One slot is always left unused
// so that "read == write" means empty.
template<typename T, uint32 elements, typename P = uint32>
class RingBuffer
{
public:
	RingBuffer() = default;

	// false (and nothing stored) when the buffer is full
	bool Push(const T& v)
	{
		std::lock_guard lock(m_lock);
		if (step(m_write) == m_read)
		{
			debugBreakpoint();
			return false;
		}
		m_data[m_write] = v;
		m_write = step(m_write);
		return true;
	}

	// default-constructed T when empty
	T Pop() requires (!std::is_array_v<T>)
	{
		std::lock_guard lock(m_lock);
		if (m_read == m_write)
			return T();
		T value = m_data[m_read];
		m_read = step(m_read);
		return value;
	}

	// claims the next write slot (no full check)
	T& GetSlot()
	{
		std::lock_guard lock(m_lock);
		T& slot = m_data[m_write];
		m_write = step(m_write);
		return slot;
	}

	// claims the next write slot and drops the oldest entry
	T& GetSlotAndAdvance()
	{
		std::lock_guard lock(m_lock);
		T& slot = m_data[m_write];
		m_write = step(m_write);
		m_read = step(m_read);
		return slot;
	}

	// drops the oldest entry, if any
	void Advance()
	{
		std::lock_guard lock(m_lock);
		if (m_read != m_write)
			m_read = step(m_read);
	}

	void Clear()
	{
		std::lock_guard lock(m_lock);
		m_read = 0;
		m_write = 0;
	}

	P GetReadPointer()
	{
		std::lock_guard lock(m_lock);
		return m_read;
	}

	P GetWritePointer()
	{
		std::lock_guard lock(m_lock);
		return m_write;
	}

	bool HasData()
	{
		std::lock_guard lock(m_lock);
		return m_read != m_write;
	}

private:
	static P step(P index)
	{
		return (P)((index + 1) % elements);
	}

	T m_data[elements];
	P m_read{ 0 };
	P m_write{ 0 };
	std::mutex m_lock;
};
