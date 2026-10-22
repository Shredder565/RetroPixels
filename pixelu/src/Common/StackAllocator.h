// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Scoped allocation of N objects on the emulated PPC stack of the current
// thread. The objects are value-initialized on construction, destroyed on
// scope exit, and the stack pointer is restored.
//
// Layout of the reserved area: a 64 byte gap below and above the objects.
// The reserved byte count is rounded the same way the HLE code always has:
//     raw = elements * sizeof(T) + 128
//     reserved = (raw / 8 + 7) * 8
// Guest code can observe the resulting stack addresses, so this must stay
// exactly as it is.
template<typename T, int count = 1>
class StackAllocator
{
	static constexpr uint32 kGuardBytes = 64;

	static sint32 ReservedSize(uint32 elementCount)
	{
		const sint32 raw = (sint32)(elementCount * sizeof(T) + kGuardBytes * 2);
		return (raw / 8 + 7) * 8;
	}

public:
	StackAllocator() : StackAllocator(1) {}

	explicit StackAllocator(const uint32 items)
		: m_numElements(count * items), m_reservedBytes(ReservedSize(count * items))
	{
		uint8* area = PPCInterpreter_PushAndReturnStackPointer(m_reservedBytes);
		m_data = new (area + kGuardBytes) T[m_numElements]();
	}

	StackAllocator(const StackAllocator&) = delete;
	StackAllocator& operator=(const StackAllocator&) = delete;

	~StackAllocator()
	{
		for (uint32 i = 0; i < m_numElements; i++)
			m_data[i].~T();
		PPCInterpreterModifyStackPointer(-m_reservedBytes);
	}

	T* GetPointer() const { return m_data; }
	uint32 GetMPTR() const { return MEMPTR<T>(m_data).GetMPTR(); }

	T* operator->() const { return m_data; }
	T* operator&() { return m_data; }

	explicit operator T*() const { return m_data; }
	explicit operator uint32() const { return GetMPTR(); }
	explicit operator bool() const { return *m_data != 0; }

	// element access, arrays only
	template<int N = count>
		requires(N > 1)
	T& operator[](const uint32 index)
	{
		return m_data[index];
	}

	// value assignment and comparison, single objects only
	template<int N = count>
		requires(N == 1)
	T& operator=(const T& value)
	{
		*m_data = value;
		return *m_data;
	}

	template<int N = count>
		requires(N == 1)
	bool operator==(const T& value) const
	{
		return *m_data == value;
	}

private:
	uint32 m_numElements;
	sint32 m_reservedBytes;
	T* m_data;
};
