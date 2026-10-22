// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Common/enumFlags.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>

// A single setting. The emulator reads settings from many threads while the
// settings dialog changes them, so every read and write is synchronised:
// small plain values (bool, numbers, enums) live in a std::atomic, anything
// else (strings) behind a reader/writer lock.
// Each setting also remembers the value it was declared with; that value is
// the default used when settings.xml has no entry for it.

namespace config_value_internal
{
	// values that fit the lock-free path
	template<typename T>
	inline constexpr bool kStoreAtomically = std::is_trivially_copyable_v<T> && std::is_copy_constructible_v<T> &&
											 std::is_move_constructible_v<T> && std::is_copy_assignable_v<T> &&
											 std::is_move_assignable_v<T>;

	template<typename T, bool TAtomic = kStoreAtomically<T>>
	class Slot;

	template<typename T>
	class Slot<T, true>
	{
	public:
		constexpr explicit Slot(const T& v) : m_current(v) {}

		T Read() const { return m_current.load(); }
		void Write(const T& v) { m_current.store(v); }

	private:
		std::atomic<T> m_current;
	};

	template<typename T>
	class Slot<T, false>
	{
	public:
		explicit Slot(const T& v) : m_current(v) {}

		T Read() const
		{
			std::shared_lock reading(m_guard);
			return m_current;
		}

		template<typename TFrom>
		void Write(TFrom&& v)
		{
			std::unique_lock writing(m_guard);
			m_current = std::forward<TFrom>(v);
		}

	private:
		mutable std::shared_mutex m_guard;
		T m_current;
	};
}

template<typename TType>
class ConfigValue
{
public:
	constexpr ConfigValue() : ConfigValue(TType{}) {}
	constexpr ConfigValue(const TType& init_value) : m_slot(init_value), m_init_value(init_value) {}

	// a copy starts with the current value of the original
	ConfigValue(const ConfigValue& other) : m_slot(other.GetValue()), m_init_value(other.m_init_value) {}

	// assignment changes the value only, the declared default stays
	ConfigValue& operator=(const ConfigValue& other)
	{
		SetValue(other.GetValue());
		return *this;
	}

	ConfigValue& operator=(const TType& v)
	{
		SetValue(v);
		return *this;
	}

	[[nodiscard]] TType GetValue() const { return m_slot.Read(); }
	void SetValue(const TType& v) { m_slot.Write(v); }

	void SetValue(std::string_view v)
		requires std::is_same_v<TType, std::string>
	{
		m_slot.Write(v);
	}

	[[nodiscard]] const TType& GetInitValue() const { return m_init_value; }

	[[nodiscard]] operator TType() const { return GetValue(); }

private:
	config_value_internal::Slot<TType> m_slot;
	const TType m_init_value;
};

// A setting restricted to [min, max]. Storing a value outside the range stores
// the declared default instead. Enums registered with ENABLE_ENUM_ITERATORS
// get their range from the registration.
template<typename TType>
class ConfigValueBounds : public ConfigValue<TType>
{
	using Value = ConfigValue<TType>;
	static constexpr bool kIterableEnum = std::is_enum_v<TType> && EnableEnumIterators<TType>::enable;

public:
	constexpr ConfigValueBounds(const TType& min, const TType& init_value, const TType& max)
		: Value(init_value), m_min_value(min), m_max_value(max)
	{
		assert(min <= init_value && init_value <= max);
	}

	constexpr ConfigValueBounds()
		requires kIterableEnum
		: ConfigValueBounds(TType{})
	{
	}

	constexpr ConfigValueBounds(const TType& init_value)
		requires kIterableEnum
		: Value(init_value), m_min_value(EnableEnumIterators<TType>::begin), m_max_value(EnableEnumIterators<TType>::rbegin)
	{
		assert(m_min_value <= init_value && init_value <= m_max_value);
	}

	ConfigValueBounds(const ConfigValueBounds&) = default;

	ConfigValueBounds& operator=(const ConfigValueBounds& other)
	{
		return *this = other.GetValue();
	}

	ConfigValueBounds& operator=(const TType& v)
	{
		Value::SetValue(InRange(v) ? v : this->GetInitValue());
		return *this;
	}

	[[nodiscard]] const TType& GetMax() const { return m_max_value; }
	[[nodiscard]] const TType& GetMin() const { return m_min_value; }

private:
	bool InRange(const TType& v) const { return !(v < m_min_value) && !(v > m_max_value); }

	const TType m_min_value;
	const TType m_max_value;
};
