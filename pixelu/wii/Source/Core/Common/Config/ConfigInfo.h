// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <mutex>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Config/Enums.h"
#include "Common/Mutex.h"
#include "Common/TypeUtils.h"

namespace Config
{
// Where a setting lives: the INI file (system), section and key. Section and key compare
// case-insensitively.
struct Location
{
  System system{};
  std::string section;
  std::string key;

  bool operator==(const Location& other) const;
  bool operator<(const Location& other) const;
};

// A value together with the config version it was read at.
template <typename T>
struct CachedValue
{
  T value;
  u64 config_version;
};

// Describes one setting: its location and default value. Each Info also remembers the last
// value Config::Get() read for it, which stays valid until the configuration changes.
template <typename T>
class Info
{
public:
  constexpr Info(Location location, T default_value)
      : m_location{std::move(location)}, m_default_value{default_value},
        m_cache{std::move(default_value), 0}
  {
  }

  Info(const Info<T>& other)
      : m_location{other.m_location}, m_default_value{other.m_default_value},
        m_cache(other.GetCachedValue())
  {
  }

  // Lets an Info<SomeEnum> be used where an Info of its underlying integer type is wanted.
  template <Common::TypedEnum<T> Enum>
  Info(const Info<Enum>& other)
      : m_location{other.GetLocation()}, m_default_value{static_cast<T>(other.GetDefaultValue())},
        m_cache(other.template GetCachedValueCasted<T>())
  {
  }

  ~Info() = default;

  // Infos are effectively constants; reassigning or moving one would need much more locking.
  Info& operator=(const Info&) = delete;
  Info& operator=(Info&&) = delete;
  Info(Info&&) = delete;

  constexpr const Location& GetLocation() const { return m_location; }
  constexpr const T& GetDefaultValue() const { return m_default_value; }

  CachedValue<T> GetCachedValue() const
  {
    std::lock_guard guard{m_cache_lock};
    return m_cache;
  }

  template <typename U>
  CachedValue<U> GetCachedValueCasted() const
  {
    std::lock_guard guard{m_cache_lock};
    return CachedValue<U>{static_cast<U>(m_cache.value), m_cache.config_version};
  }

  // Stores the value unless the cache already holds one from the same or a newer version.
  void TryToSetCachedValue(CachedValue<T> new_value) const
  {
    std::lock_guard guard{m_cache_lock};
    if (m_cache.config_version < new_value.config_version)
      m_cache = std::move(new_value);
  }

private:
  Location m_location;
  T m_default_value;

  mutable CachedValue<T> m_cache;
  // Held only for the copy in and out of m_cache, and rarely contended.
  mutable Common::SpinMutex m_cache_lock;
};
}  // namespace Config
