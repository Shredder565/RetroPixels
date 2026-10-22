// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <type_traits>
#include "betype.h"

using MPTR = uint32; // address in the guest (PowerPC) address space

#define MPTR_NULL (0)

using VAddr = uint32; // virtual address
using PAddr = uint32; // physical address

extern uint8* memory_base;
extern uint8* PPCInterpreterGetStackPointer();
extern uint8* PPCInterpreter_PushAndReturnStackPointer(sint32 offset);
extern void PPCInterpreterModifyStackPointer(sint32 offset);

// tag base so templates can detect MEMPTR<...> with std::is_base_of
class MEMPTRBase
{
};

// A guest pointer: a 32-bit big-endian guest address that converts to and
// from host pointers into the guest memory mapping. Address 0 is null.
// Note that operator-(MEMPTR) returns the difference in bytes.
template<typename T>
class MEMPTR : MEMPTRBase
{
public:
	constexpr MEMPTR() noexcept : m_value(0) {}
	explicit constexpr MEMPTR(uint32 offset) noexcept : m_value(offset) {}
	explicit constexpr MEMPTR(const uint32be& offset) noexcept : m_value(offset) {}
	constexpr MEMPTR(std::nullptr_t) noexcept : m_value(0) {}
	MEMPTR(T* ptr) noexcept : m_value(toGuest(ptr)) {}

	constexpr MEMPTR(const MEMPTR&) noexcept = default;
	constexpr MEMPTR& operator=(const MEMPTR&) noexcept = default;

	// assignment from a guest address, from null or from a host pointer into guest memory
	constexpr MEMPTR& operator=(const uint32& offset) noexcept { return Store(offset); }
	constexpr MEMPTR& operator=(std::nullptr_t) noexcept { return Store(0); }
	MEMPTR& operator=(T* ptr) noexcept { return Store(toGuest(ptr)); }

	// atomically replaces the pointer with newPtr if it currently equals comparePtr
	bool atomic_compare_exchange(T* comparePtr, T* newPtr) noexcept
	{
		uint32be expected = toGuest(comparePtr);
		const uint32be desired = toGuest(newPtr);
		return reinterpret_cast<std::atomic<uint32be>*>(&m_value)->compare_exchange_strong(expected, desired);
	}

	explicit constexpr operator bool() const noexcept { return m_value != 0; }

	constexpr operator T*() const noexcept { return GetPtr(); }

	template<typename X>
	explicit operator MEMPTR<X>() const noexcept
	{
		return MEMPTR<X>(m_value);
	}

	// distance in bytes
	sint32 operator-(const MEMPTR& ptr) const noexcept
		requires(!std::is_void_v<T>)
	{
		return static_cast<sint32>(GetMPTR() - ptr.GetMPTR());
	}

	// element-wise pointer arithmetic
	MEMPTR operator+(sint32 v) const noexcept
		requires(!std::is_void_v<T>)
	{
		return MEMPTR((uint32)(GetMPTR() + v * sizeof(T)));
	}

	MEMPTR operator-(sint32 v) const noexcept
		requires(!std::is_void_v<T>)
	{
		return MEMPTR((uint32)(GetMPTR() - v * sizeof(T)));
	}

	MEMPTR& operator+=(sint32 v) noexcept
		requires(!std::is_void_v<T>)
	{
		m_value = (uint32)(GetMPTR() + v * sizeof(T));
		return *this;
	}

	template<typename Q = T>
		requires(!std::is_void_v<Q>)
	Q& operator*() const noexcept
	{
		return *GetPtr();
	}

	constexpr T* operator->() const noexcept { return GetPtr(); }

	template<typename Q = T>
		requires(!std::is_void_v<Q>)
	Q& operator[](int index) const noexcept
	{
		return GetPtr()[index];
	}

	T* GetPtr() const noexcept
	{
		const uint32 addr = m_value;
		return addr == 0 ? nullptr : (T*)(memory_base + addr);
	}

	template<typename C>
	C* GetPtr() const noexcept
	{
		return static_cast<C*>(GetPtr());
	}

	[[nodiscard]] constexpr uint32 GetMPTR() const noexcept { return m_value.value(); }
	[[nodiscard]] constexpr const uint32be& GetBEValue() const noexcept { return m_value; }
	[[nodiscard]] constexpr bool IsNull() const noexcept { return m_value == 0; }

private:
	static uint32 toGuest(T* ptr) noexcept
	{
		if (!ptr)
			return 0;
		cemu_assert_debug((uint8*)ptr >= memory_base && (uint8*)ptr <= memory_base + 0x100000000);
		return (uint32)((uintptr_t)ptr - (uintptr_t)memory_base);
	}

	constexpr MEMPTR& Store(uint32 guestAddress) noexcept
	{
		m_value = guestAddress;
		return *this;
	}

	uint32be m_value;
};

static_assert(sizeof(MEMPTR<void*>) == sizeof(uint32be));
static_assert(std::is_trivially_copyable_v<MEMPTR<void*>>);

#include "StackAllocator.h"
#include "SysAllocator.h"

// printed as hex guest address
template<typename T>
struct fmt::formatter<MEMPTR<T>> : formatter<string_view>
{
	template<typename FormatContext>
	auto format(const MEMPTR<T>& v, FormatContext& ctx) const -> format_context::iterator
	{
		return fmt::format_to(ctx.out(), "{:#x}", v.GetMPTR());
	}
};
