// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/CPUThreadConfigCallback.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/Config/Config.h"
#include "Core/Core.h"

namespace CPUThreadConfigCallback
{
namespace
{
struct Registration
{
  ConfigChangedCallbackID id;
  Config::ConfigChangedCallback callback;
};

// Only the CPU thread (or a thread acting as it) touches the list.
std::vector<Registration> s_registrations;
size_t s_last_issued_id = std::numeric_limits<size_t>::max();

// Set when a setting changed on some other thread.
std::atomic<bool> s_change_pending = false;

void InvokeAll()
{
  for (const Registration& registration : s_registrations)
    registration.callback();
}

void OnAnyConfigChange()
{
  const bool on_cpu_thread = Core::IsCPUThread();
  s_change_pending.store(!on_cpu_thread, std::memory_order_relaxed);
  if (on_cpu_thread)
    InvokeAll();
}

void HookConfigSystemOnce()
{
  // The hook stays for the rest of the process.
  [[maybe_unused]] static const Config::ConfigChangedCallbackID hook =
      Config::AddConfigChangedCallback(&OnAnyConfigChange);
}
}  // namespace

ConfigChangedCallbackID AddConfigChangedCallback(Config::ConfigChangedCallback func)
{
  HookConfigSystemOnce();

  const ConfigChangedCallbackID id{++s_last_issued_id};
  s_registrations.push_back(Registration{id, std::move(func)});
  return id;
}

void RemoveConfigChangedCallback(ConfigChangedCallbackID callback_id)
{
  const auto match = std::ranges::find(s_registrations, callback_id, &Registration::id);
  if (match != s_registrations.end())
    s_registrations.erase(match);
}

void CheckForConfigChanges()
{
  DEBUG_ASSERT(Core::IsCPUThread());

  if (s_change_pending.exchange(false, std::memory_order_relaxed))
    InvokeAll();
}
}  // namespace CPUThreadConfigCallback
