// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

#include "Common/Config/ConfigInfo.h"
#include "Common/Config/Enums.h"
#include "Common/StringUtil.h"

namespace Config
{
// Passing a DefaultState as the value removes the setting from the layer.
struct DefaultState
{
  friend constexpr bool operator==(DefaultState, DefaultState) { return true; }
};

namespace detail
{
// Converts a stored string into a setting's type (nullopt if it does not parse).
template <typename T>
requires(!Common::Enum<T>)
std::optional<T> TryParse(const std::string& str_value)
{
  T parsed;
  if (::TryParse(str_value, &parsed))
    return parsed;
  return std::nullopt;
}

template <Common::Enum T>
std::optional<T> TryParse(const std::string& str_value)
{
  if (const auto number = TryParse<std::underlying_type_t<T>>(str_value))
    return static_cast<T>(*number);
  return std::nullopt;
}

template <>
inline std::optional<std::string> TryParse(const std::string& str_value)
{
  return str_value;
}
}  // namespace detail

template <typename T>
class Info;

class Layer;

// All values of a layer, as strings. An empty optional marks a key deleted since loading (so
// the loader knows to remove it from the file).
using LayerMap = std::map<Location, std::optional<std::string>>;

// Reads a layer's values from somewhere (INI files, a game's settings, a movie) and writes them
// back.
class ConfigLayerLoader
{
public:
  explicit ConfigLayerLoader(LayerType layer);
  virtual ~ConfigLayerLoader();
  virtual void Load(Layer* config_layer) = 0;
  virtual void Save(Layer* config_layer) = 0;

  LayerType GetLayer() const;

private:
  const LayerType m_layer;
};

// The entries of one section of a layer, as an iterable range.
class Section
{
public:
  using iterator = LayerMap::iterator;

  Section(iterator first, iterator last) : m_begin{first}, m_end{last} {}

  iterator end() const { return m_end; }
  iterator begin() const { return m_begin; }

private:
  iterator m_begin;
  iterator m_end;
};

// The same over a const layer.
class ConstSection
{
public:
  using iterator = LayerMap::const_iterator;

  ConstSection(iterator first, iterator last) : m_begin{first}, m_end{last} {}

  iterator end() const { return m_end; }
  iterator begin() const { return m_begin; }

private:
  iterator m_begin;
  iterator m_end;
};

class Layer
{
public:
  explicit Layer(LayerType layer);
  // Loads the layer right away and saves it (if changed) when destroyed.
  explicit Layer(std::unique_ptr<ConfigLayerLoader> loader);
  virtual ~Layer();

  bool Exists(const Location& location) const;
  // Returns whether there was a value to delete.
  bool DeleteKey(const Location& location);
  void DeleteAllKeys();

  template <typename T>
  T Get(const Info<T>& config_info) const
  {
    return Get<T>(config_info.GetLocation()).value_or(config_info.GetDefaultValue());
  }

  template <typename T>
  std::optional<T> Get(const Location& location) const
  {
    const auto entry = m_map.find(location);
    if (entry == m_map.end() || !entry->second)
      return std::nullopt;
    return detail::TryParse<T>(*entry->second);
  }

  // The Set functions return whether the layer changed.

  bool Set(const Location& location, std::string new_value)
  {
    if (const auto entry = m_map.find(location); entry != m_map.end())
    {
      if (entry->second == new_value)
        return false;
      entry->second = std::move(new_value);
    }
    else
    {
      m_map.emplace(location, std::move(new_value));
    }
    MarkAsDirty();
    return true;
  }

  template <typename T>
  bool Set(const Location& location, const T& value)
  {
    return Set(location, ValueToString(value));
  }

  // DefaultState removes the key, so the default applies again.
  bool Set(const Location& location, DefaultState) { return DeleteKey(location); }
  template <typename T>
  bool Set(const Info<T>& config_info, DefaultState)
  {
    return Set(config_info.GetLocation(), DefaultState{});
  }

  // (The value's type is not deduced, so it converts to the setting's type.)
  template <typename T>
  bool Set(const Info<T>& config_info, const std::common_type_t<T>& value)
  {
    return Set(config_info.GetLocation(), value);
  }

  void MarkAsDirty() { m_is_dirty = true; }

  Section GetSection(System system, const std::string& section);
  ConstSection GetSection(System system, const std::string& section) const;

  void Load();
  void Save();

  LayerType GetLayer() const;
  const LayerMap& GetLayerMap() const;

protected:
  bool m_is_dirty = false;
  LayerMap m_map;
  const LayerType m_layer;
  std::unique_ptr<ConfigLayerLoader> m_loader;
};
}  // namespace Config
