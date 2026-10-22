// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "Common/Config/ConfigInfo.h"
#include "Common/Config/Enums.h"
#include "Common/Config/Layer.h"

// The layered configuration system. Settings are described by Config::Info objects; reading one
// gives the value from the highest-priority layer that has it (see SEARCH_ORDER).
namespace Config
{
struct ConfigChangedCallbackID
{
  size_t id = std::numeric_limits<size_t>::max();

  bool operator==(const ConfigChangedCallbackID&) const = default;
};

using ConfigChangedCallback = std::function<void()>;

// Layers. Adding a layer replaces any existing layer of the same type.
void AddLayer(std::unique_ptr<ConfigLayerLoader> loader);
std::shared_ptr<Layer> GetLayer(LayerType layer);
void RemoveLayer(LayerType layer);

// Callbacks run (on whichever thread made the change) whenever the configuration changes.
// Keep the returned ID to remove the callback again.
[[nodiscard]] ConfigChangedCallbackID AddConfigChangedCallback(ConfigChangedCallback func);
void RemoveConfigChangedCallback(ConfigChangedCallbackID callback_id);
void OnConfigChanged();

// Increases with every change; used to invalidate the values cached in Info objects.
u64 GetConfigVersion();

// Loads or saves every layer through its loader.
void Load();
void Save();

void Init();
void Shutdown();
void ClearCurrentRunLayer();

const std::string& GetSystemName(System system);
std::optional<System> GetSystemFromName(const std::string& system);
const std::string& GetLayerName(LayerType layer);
// The layer the effective value of a setting comes from (Base if no layer has it).
LayerType GetActiveLayerForConfig(const Location&);

// The effective value of a setting as stored, if any layer has it.
std::optional<std::string> GetAsString(const Location&);

template <typename T>
T GetUncached(const Info<T>& info)
{
  const std::optional<std::string> stored = GetAsString(info.GetLocation());
  if (!stored.has_value())
    return info.GetDefaultValue();
  return detail::TryParse<T>(*stored).value_or(info.GetDefaultValue());
}

// The effective value of a setting.
template <typename T>
T Get(const Info<T>& info)
{
  CachedValue<T> cached = info.GetCachedValue();
  const u64 current_version = GetConfigVersion();
  if (cached.config_version < current_version)
  {
    cached.value = GetUncached(info);
    cached.config_version = current_version;
    info.TryToSetCachedValue(cached);
  }
  return std::move(cached.value);
}

// The value in one particular layer (LayerType::Meta means the effective value).
template <typename T>
T Get(LayerType layer, const Info<T>& info)
{
  if (layer == LayerType::Meta)
    return Get(info);
  return GetLayer(layer)->Get(info);
}

// The value from a game's own settings layer if one is given, otherwise the effective value.
template <typename T>
T Get(const Layer* game_layer, const Info<T>& setting)
{
  if (game_layer == nullptr)
    return Get(setting);
  return game_layer->Get(setting);
}

// The layer the effective value of `info` comes from.
template <typename T>
LayerType GetActiveLayerForConfig(const Info<T>& info)
{
  return GetActiveLayerForConfig(info.GetLocation());
}

// The value stored in the base layer (or the default when it has none).
template <typename T>
T GetBase(const Info<T>& info)
{
  return GetLayer(LayerType::Base)->Get(info);
}

template <typename InfoT, typename ValueT>
void Set(LayerType layer, const InfoT& info, const ValueT& value)
{
  const bool changed = GetLayer(layer)->Set(info, value);
  if (changed)
    OnConfigChanged();
}

template <typename InfoT, typename ValueT>
void SetBase(const Info<InfoT>& info, const ValueT& value)
{
  Set(LayerType::Base, info, value);
}

template <typename InfoT, typename ValueT>
void SetCurrent(const Info<InfoT>& info, const ValueT& value)
{
  Set(LayerType::CurrentRun, info, value);
}

// Changes the Base layer if that is where the effective value comes from, otherwise only the
// current run (so that game-specific or temporary overrides are not written to the user's INI).
template <typename InfoT, typename ValueT>
void SetBaseOrCurrent(const Info<InfoT>& info, const ValueT& value)
{
  const LayerType target =
      GetActiveLayerForConfig(info) == LayerType::Base ? LayerType::Base : LayerType::CurrentRun;
  Set(target, info, value);
}

template <typename T>
void DeleteKey(LayerType layer, const Info<T>& info)
{
  if (GetLayer(layer)->DeleteKey(info.GetLocation()))
    OnConfigChanged();
}

template <typename T>
bool IsDefaultValue(const Info<T>& info)
{
  return Get(info) == info.GetDefaultValue();
}

// While at least one guard exists, change callbacks are held back; they run once when the last
// guard goes away.
class ConfigChangeCallbackGuard
{
public:
  ConfigChangeCallbackGuard();
  ~ConfigChangeCallbackGuard();

  ConfigChangeCallbackGuard(const ConfigChangeCallbackGuard&) = delete;
  ConfigChangeCallbackGuard& operator=(const ConfigChangeCallbackGuard&) = delete;
};
}  // namespace Config
