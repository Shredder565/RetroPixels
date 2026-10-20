// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <bit>
#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"

namespace Common
{
// Splits a pointer-to-member type `M O::*` (data or function member) into its parts.
template <typename>
struct MemberPointerInfo;

template <typename M, typename O>
struct MemberPointerInfo<M O::*>
{
  using MemberType = M;
  using ObjectType = O;
};

// The member's type and the class that declares it, for a given member pointer value.
// (For a member inherited from a base class, ObjectType is the base.)
template <auto member_pointer>
using MemberType = typename MemberPointerInfo<decltype(member_pointer)>::MemberType;

template <auto member_pointer>
using ObjectType = typename MemberPointerInfo<decltype(member_pointer)>::ObjectType;

// True when exactly `count` types are given and each converts to T.
template <typename T, std::size_t count, typename... Ts>
struct IsNOf
    : std::bool_constant<sizeof...(Ts) == count && (std::is_convertible_v<Ts, T> && ...)>
{
};

static_assert(IsNOf<int, 0>::value);
static_assert(IsNOf<int, 2, int, short>::value);
static_assert(!IsNOf<int, 1, int, int>::value);
static_assert(!IsNOf<int, 1, char*>::value);

// Storage for one T whose lifetime the owner controls: nothing is constructed or destroyed
// automatically. Every Construct() must be paired with a Destroy().
template <typename T>
class ManuallyConstructedValue
{
public:
  template <typename... Args>
  T& Construct(Args&&... args)
  {
    static_assert(sizeof(ManuallyConstructedValue) == sizeof(T));
    T* object = ::new (static_cast<void*>(&m_storage.object)) T(std::forward<Args>(args)...);
    return *object;
  }

  void Destroy() { m_storage.object.~T(); }

  T* Ptr() { return &m_storage.object; }
  const T* Ptr() const { return &m_storage.object; }
  T& Ref() { return m_storage.object; }
  const T& Ref() const { return m_storage.object; }

  T* operator->() { return Ptr(); }
  const T* operator->() const { return Ptr(); }
  T& operator*() { return Ref(); }
  const T& operator*() const { return Ref(); }

private:
  union Storage
  {
    Storage() {}
    ~Storage() {}
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    T object;
  } m_storage;
};

template <typename T>
concept Enum = std::is_enum_v<T>;

// An enum whose underlying type is exactly `Underlying`.
template <typename T, typename Underlying>
concept TypedEnum = std::is_same_v<std::underlying_type_t<T>, Underlying>;

template <typename T>
concept BooleanEnum = TypedEnum<T, bool>;

// The unsigned integer type with the same size as T (1, 2, 4 or 8 bytes).
template <typename T>
requires(sizeof(T) <= sizeof(u64) && std::has_single_bit(sizeof(T)))
using MakeUnsignedSameSize =
    std::tuple_element_t<MathUtil::IntLog2(sizeof(T)), std::tuple<u8, u16, u32, u64>>;

// u64 stays u64; every smaller unsigned type widens to u32.
template <std::unsigned_integral T>
using MakeAtLeastU32 = std::conditional_t<(sizeof(T) > sizeof(u32)), u64, u32>;
}  // namespace Common
