// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Config/Config.h"

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace Config
{
namespace
{
struct CallbackEntry
{
  ConfigChangedCallbackID id;
  ConfigChangedCallback function;
};

// Layers, guarded by a reader/writer lock.
std::shared_mutex s_layers_lock;
std::map<LayerType, std::shared_ptr<Layer>> s_layers;

// Callbacks. The lock is recursive so that a callback may add or remove callbacks.
std::recursive_mutex s_callbacks_lock;
std::vector<CallbackEntry> s_callbacks;
size_t s_next_callback_id = 0;

std::atomic<u32> s_guard_count{0};
std::atomic<u64> s_version{0};

// Names used for INI files and shown in the UI.
const std::array<std::pair<System, std::string>, 12> SYSTEM_NAMES{{
    {System::Main, "Dolphin"},
    {System::SYSCONF, "SYSCONF"},
    {System::GCPad, "GCPad"},
    {System::WiiPad, "Wiimote"},
    {System::GCKeyboard, "GCKeyboard"},
    {System::GFX, "Graphics"},
    {System::Logger, "Logger"},
    {System::DualShockUDPClient, "DualShockUDPClient"},
    {System::FreeLook, "FreeLook"},
    {System::Session, "Session"},
    {System::GameSettingsOnly, "GameSettingsOnly"},
    {System::Achievements, "Achievements"},
}};

const std::array<std::pair<LayerType, std::string>, 7> LAYER_NAMES{{
    {LayerType::Base, "Base"},
    {LayerType::CommandLine, "Command Line"},
    {LayerType::GlobalGame, "Global GameINI"},
    {LayerType::LocalGame, "Local GameINI"},
    {LayerType::Movie, "Movie"},
    {LayerType::Netplay, "Netplay"},
    {LayerType::CurrentRun, "Current Run"},
}};

void PutLayer(std::shared_ptr<Layer> layer)
{
  {
    std::unique_lock lock(s_layers_lock);
    const LayerType type = layer->GetLayer();
    s_layers[type] = std::move(layer);
  }
  OnConfigChanged();
}

// Calls `visit` for each existing layer in SEARCH_ORDER until it returns true; returns the type
// of that layer. Must be called with s_layers_lock held.
template <typename Visitor>
std::optional<LayerType> FindInSearchOrder(Visitor&& visit)
{
  for (const LayerType type : SEARCH_ORDER)
  {
    const auto it = s_layers.find(type);
    if (it != s_layers.end() && visit(*it->second))
      return type;
  }
  return std::nullopt;
}
}  // namespace

void AddLayer(std::unique_ptr<ConfigLayerLoader> loader)
{
  PutLayer(std::make_shared<Layer>(std::move(loader)));
}

std::shared_ptr<Layer> GetLayer(LayerType layer)
{
  std::shared_lock lock(s_layers_lock);
  const auto it = s_layers.find(layer);
  if (it == s_layers.end())
    return nullptr;
  return it->second;
}

void RemoveLayer(LayerType layer)
{
  {
    std::unique_lock lock(s_layers_lock);
    s_layers.erase(layer);
  }
  OnConfigChanged();
}

ConfigChangedCallbackID AddConfigChangedCallback(ConfigChangedCallback func)
{
  std::lock_guard lock(s_callbacks_lock);
  const ConfigChangedCallbackID id{s_next_callback_id++};
  s_callbacks.push_back(CallbackEntry{id, std::move(func)});
  return id;
}

void RemoveConfigChangedCallback(ConfigChangedCallbackID callback_id)
{
  std::lock_guard lock(s_callbacks_lock);
  for (auto it = s_callbacks.begin(); it != s_callbacks.end(); ++it)
  {
    if (it->id == callback_id)
    {
      s_callbacks.erase(it);
      return;
    }
  }
}

void OnConfigChanged()
{
  // Bump the version even while callbacks are held back, so cached values are never stale.
  s_version.fetch_add(1, std::memory_order_relaxed);

  if (s_guard_count.load() != 0)
    return;

  std::lock_guard lock(s_callbacks_lock);
  // Indexed loop: a callback may add or remove entries while we walk the list.
  for (size_t i = 0; i < s_callbacks.size(); ++i)
  {
    const ConfigChangedCallback callback = s_callbacks[i].function;
    callback();
  }
}

u64 GetConfigVersion()
{
  return s_version.load(std::memory_order_relaxed);
}

void Load()
{
  {
    std::shared_lock lock(s_layers_lock);
    for (const auto& [type, layer] : s_layers)
      layer->Load();
  }
  OnConfigChanged();
}

void Save()
{
  {
    std::shared_lock lock(s_layers_lock);
    for (const auto& [type, layer] : s_layers)
      layer->Save();
  }
  OnConfigChanged();
}

void Init()
{
  ClearCurrentRunLayer();
}

void Shutdown()
{
  std::unique_lock lock(s_layers_lock);
  s_layers.clear();
}

void ClearCurrentRunLayer()
{
  std::unique_lock lock(s_layers_lock);
  s_layers[LayerType::CurrentRun] = std::make_shared<Layer>(LayerType::CurrentRun);
}

const std::string& GetSystemName(System system)
{
  for (const auto& [value, name] : SYSTEM_NAMES)
  {
    if (value == system)
      return name;
  }
  static const std::string unknown;
  return unknown;
}

std::optional<System> GetSystemFromName(const std::string& system)
{
  for (const auto& [value, name] : SYSTEM_NAMES)
  {
    if (name == system)
      return value;
  }
  return std::nullopt;
}

const std::string& GetLayerName(LayerType layer)
{
  for (const auto& [value, name] : LAYER_NAMES)
  {
    if (value == layer)
      return name;
  }
  static const std::string unknown;
  return unknown;
}

LayerType GetActiveLayerForConfig(const Location& config)
{
  std::shared_lock lock(s_layers_lock);
  const auto found = FindInSearchOrder([&](const Layer& layer) { return layer.Exists(config); });
  return found.value_or(LayerType::Base);
}

std::optional<std::string> GetAsString(const Location& config)
{
  std::shared_lock lock(s_layers_lock);
  std::optional<std::string> value;
  FindInSearchOrder([&](const Layer& layer) {
    value = layer.Get<std::string>(config);
    return value.has_value();
  });
  return value;
}

ConfigChangeCallbackGuard::ConfigChangeCallbackGuard()
{
  s_guard_count.fetch_add(1);
}

ConfigChangeCallbackGuard::~ConfigChangeCallbackGuard()
{
  if (s_guard_count.fetch_sub(1) == 1)
    OnConfigChanged();
}
}  // namespace Config
