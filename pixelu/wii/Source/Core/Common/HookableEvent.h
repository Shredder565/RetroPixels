// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Common/Logging/Log.h"

namespace Common
{
// Handle type for a registered event listener. Destroying the handle unregisters the listener.
// The base is abstract (pure virtual destructor) so it can only be used through the pointer.
struct HookBase
{
  virtual ~HookBase() = 0;
};
inline HookBase::~HookBase() = default;

using EventHook = std::unique_ptr<HookBase>;

// An event that any number of listeners can subscribe to:
//
//   HookableEvent<int> on_value;
//   EventHook hook = on_value.Register([](int v) { ... });
//   on_value.Trigger(5);
//
// A listener registered while the event is being triggered is called during that trigger; one
// unregistered while it is being triggered is skipped and freed once the trigger is over.
// Handles may outlive the event.
template <typename... CallbackArgs>
class HookableEvent
{
public:
  using CallbackType = std::function<void(CallbackArgs...)>;

  [[nodiscard]] EventHook Register(CallbackType callback)
  {
    DEBUG_LOG_FMT(COMMON, "Registering event hook handler");
    std::lock_guard guard(m_shared->mutex);
    auto entry = std::make_unique<Entry>(std::move(callback));
    Entry* const raw = entry.get();
    m_shared->entries.push_back(std::move(entry));
    return std::make_unique<Handle>(m_shared, raw);
  }

  void Trigger(const CallbackArgs&... args)
  {
    std::lock_guard guard(m_shared->mutex);
    ++m_shared->trigger_depth;

    // Index loop: callbacks may append entries while we iterate.
    for (std::size_t i = 0; i < m_shared->entries.size(); ++i)
    {
      Entry& entry = *m_shared->entries[i];
      if (!entry.removed)
        entry.callback(args...);
    }

    if (--m_shared->trigger_depth == 0)
      std::erase_if(m_shared->entries, [](const auto& e) { return e->removed; });
  }

private:
  struct Entry
  {
    explicit Entry(CallbackType cb) : callback(std::move(cb)) {}
    const CallbackType callback;
    bool removed = false;
  };

  struct Shared
  {
    std::recursive_mutex mutex;  // recursive: callbacks may register, unregister or trigger
    std::vector<std::unique_ptr<Entry>> entries;
    int trigger_depth = 0;
  };

  struct Handle final : HookBase
  {
    Handle(const std::shared_ptr<Shared>& shared, Entry* entry) : m_shared(shared), m_entry(entry)
    {
    }

    ~Handle() override
    {
      const std::shared_ptr<Shared> shared = m_shared.lock();
      if (!shared)
      {
        DEBUG_LOG_FMT(COMMON, "Handler outlived event hook");
        return;
      }

      DEBUG_LOG_FMT(COMMON, "Removing event hook handler");
      std::lock_guard guard(shared->mutex);
      if (shared->trigger_depth > 0)
      {
        m_entry->removed = true;  // Trigger() frees it when done
        return;
      }
      std::erase_if(shared->entries, [this](const auto& e) { return e.get() == m_entry; });
    }

    const std::weak_ptr<Shared> m_shared;
    Entry* const m_entry;
  };

  std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
};
}  // namespace Common
