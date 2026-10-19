// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <new>

// Object pool that grows in batches and never returns memory to the system.
// allocObj() constructs the object, freeObj() destroys it and keeps the slot.
template<typename T>
class MemoryPool
{
	static_assert(sizeof(T) >= sizeof(void*), "a free slot stores the link to the next free slot");

public:
	MemoryPool(int allocationGranularity) : m_batchSize(allocationGranularity) {}

	template<typename... TArgs>
	T* allocObj(TArgs&&... args)
	{
		if (!m_freeHead)
			addBatch();
		void* slot = m_freeHead;
		m_freeHead = *static_cast<void**>(slot);
		return new (slot) T(std::forward<TArgs>(args)...);
	}

	void freeObj(T* obj)
	{
		obj->~T();
		releaseSlot(obj);
	}

private:
	void releaseSlot(void* slot)
	{
		*static_cast<void**>(slot) = m_freeHead;
		m_freeHead = slot;
	}

	void addBatch()
	{
		T* batch = static_cast<T*>(::operator new(sizeof(T) * (size_t)m_batchSize, std::nothrow));
		for (int i = 0; i < m_batchSize; i++)
			releaseSlot(batch + i);
		m_totalSlots += m_batchSize;
	}

	void* m_freeHead{ nullptr };
	int m_batchSize;
	int m_totalSlots{ 0 };
};

// Pool of objects that stay constructed. Each slot is default-constructed once
// when the pool grows; acquireObj()/releaseObj() only hand slots out and take
// them back, the caller resets state as needed.
template<typename T>
class MemoryPoolPermanentObjects
{
	struct Slot
	{
		T value;
		Slot* nextFree;
	};

public:
	MemoryPoolPermanentObjects(int allocationGranularity) : m_batchSize(allocationGranularity) {}

	template<typename... TArgs>
	T* acquireObj(TArgs&&... args)
	{
		if (!m_freeHead)
			addBatch();
		Slot* slot = m_freeHead;
		m_freeHead = slot->nextFree;
		return &slot->value;
	}

	void releaseObj(T* obj)
	{
		Slot* slot = reinterpret_cast<Slot*>(reinterpret_cast<uint8*>(obj) - offsetof(Slot, value));
		slot->nextFree = m_freeHead;
		m_freeHead = slot;
	}

private:
	void addBatch()
	{
		Slot* batch = static_cast<Slot*>(::operator new(sizeof(Slot) * (size_t)m_batchSize, std::nothrow));
		for (int i = 0; i < m_batchSize; i++)
		{
			new (&batch[i].value) T();
			batch[i].nextFree = m_freeHead;
			m_freeHead = batch + i;
		}
		m_totalSlots += m_batchSize;
	}

	Slot* m_freeHead{ nullptr };
	int m_batchSize;
	int m_totalSlots{ 0 };
};
