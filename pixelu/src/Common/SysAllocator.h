// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <type_traits>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <initializer_list>

// Static objects that live in guest memory.
//
// A SysAllocator is normally declared at namespace scope. Until the guest
// memory exists its contents are kept in a host-side staging copy; once the
// system area is set up, SysAllocatorContainer::Initialize() gives every
// registered object its guest memory (in registration order, which is the
// static construction order) and moves the staged contents there.

uint32 coreinit_allocFromSysArea(uint32 size, uint32 alignment);
class SysAllocatorBase;

// 1 = place a marker after every allocation and check it on access
#define SYSALLOCATOR_GUARDS 0

class SysAllocatorContainer
{
public:
	void Initialize();
	void PushSysAllocator(SysAllocatorBase* base);

	static SysAllocatorContainer& GetInstance();

private:
	std::vector<SysAllocatorBase*> m_entries;
};

class SysAllocatorBase
{
	friend class SysAllocatorContainer;

public:
	SysAllocatorBase(); // registers itself with the container
	virtual ~SysAllocatorBase() = default;

private:
	virtual void Initialize() = 0;
};

namespace sysalloc_detail
{
	constexpr uint32 kGuardValue = 0x112A33C4;
#if SYSALLOCATOR_GUARDS
	constexpr uint32 kGuardBytes = 4;
#else
	constexpr uint32 kGuardBytes = 0;
#endif
}

// array of count elements (or a single object with a custom alignment)
template<typename T, size_t count = 1, size_t alignment = 32>
class SysAllocator : public SysAllocatorBase
{
public:
	SysAllocator()
	{
		m_staging.resize(count);
	}

	// leading elements; the rest are value-initialized
	SysAllocator(std::initializer_list<T> values)
	{
		m_staging.reserve(std::max(count, values.size()));
		m_staging.assign(values.begin(), values.end());
		if (m_staging.size() < count)
			m_staging.resize(count, T());
	}

	// a string literal, including its terminator; the rest is zero
	template<size_t N>
	SysAllocator(const char (&str)[N])
	{
		m_staging.assign(str, str + std::min(N, count));
		if (m_staging.size() < count)
			m_staging.resize(count, T());
	}

	constexpr uint32 GetCount() const { return count; }
	constexpr size_t GetByteSize() const { return count * sizeof(T); }

	T* GetPtr() const
	{
		checkGuard();
		return m_sysMem.GetPtr();
	}

	uint32 GetMPTR() const
	{
		checkGuard();
		return m_sysMem.GetMPTR();
	}

	T& operator*() { return *GetPtr(); }
	operator T*() { return GetPtr(); }
	operator void*() { return GetPtr(); }

	SysAllocator operator+(const SysAllocator& other) { return SysAllocator(GetMPTR() + other.GetMPTR()); }
	SysAllocator operator-(const SysAllocator& other) { return SysAllocator(GetMPTR() - other.GetMPTR()); }

	T* operator+(sint32 v) { return GetPtr() + v; }
	T* operator-(sint32 v) { return GetPtr() - v; }

	// element access for arrays (not bool); reads the staging copy until the
	// guest memory has been assigned
	T& operator[](int index)
		requires(count != 1) && (!std::is_same_v<T, bool>)
	{
		if (m_sysMem.IsNull())
			return m_staging[index];
		return m_sysMem[index];
	}

private:
	explicit SysAllocator(uint32 guestAddress) : m_sysMem(guestAddress) {}

	void checkGuard() const
	{
#if SYSALLOCATOR_GUARDS
		cemu_assert(*(uint32*)((uint8*)m_sysMem.GetPtr() + sizeof(T) * count) == sysalloc_detail::kGuardValue);
#endif
	}

	void Initialize() override
	{
		if (!m_sysMem.IsNull())
			return;
		m_sysMem = coreinit_allocFromSysArea((uint32)(sizeof(T) * count) + sysalloc_detail::kGuardBytes, (uint32)alignment);
		memcpy((void*)m_sysMem.GetPtr(), m_staging.data(), sizeof(T) * count);
#if SYSALLOCATOR_GUARDS
		*(uint32*)((uint8*)m_sysMem.GetPtr() + sizeof(T) * count) = sysalloc_detail::kGuardValue;
#endif
		m_staging.clear();
	}

	MEMPTR<T> m_sysMem;
	std::vector<T> m_staging;
};

template<size_t N>
SysAllocator(const char (&str)[N]) -> SysAllocator<char, N>;

// single object with the default alignment
template<typename T>
class SysAllocator<T, 1> : public SysAllocatorBase
{
public:
	SysAllocator() : m_staging{} {}
	SysAllocator(const T& value) : m_staging(value) {}

	// writes to guest memory (only valid after initialization)
	SysAllocator& operator=(const T& value)
	{
		*m_sysMem = value;
		return *this;
	}

	operator T() { return *GetPtr(); }
	operator T*() { return GetPtr(); }

	T* GetPtr() const { return m_sysMem.GetPtr(); }
	uint32 GetMPTR() const { return m_sysMem.GetMPTR(); }

	T* operator&() { return GetPtr(); }
	T* operator->() const { return GetPtr(); }

private:
	void Initialize() override
	{
		if (!m_sysMem.IsNull())
			return;
		m_sysMem = coreinit_allocFromSysArea((uint32)sizeof(T), 32);
		*m_sysMem = m_staging;
	}

	MEMPTR<T> m_sysMem;
	T m_staging;
};
