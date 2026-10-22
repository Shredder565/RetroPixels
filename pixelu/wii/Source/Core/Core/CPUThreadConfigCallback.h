// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <limits>

#include "Common/Config/Config.h"

// Config-changed callbacks that always run on the CPU thread.
//
// Config::AddConfigChangedCallback callbacks run on whichever thread changed
// the setting. Callbacks registered here instead run right away when the change
// happens on the CPU thread, and otherwise at the CPU thread's next call to
// CheckForConfigChanges().
namespace CPUThreadConfigCallback
{
struct ConfigChangedCallbackID
{
  size_t id = std::numeric_limits<size_t>::max();

  bool operator==(const ConfigChangedCallbackID&) const = default;
};

// Keep the returned ID to remove the callback again.
[[nodiscard]] ConfigChangedCallbackID AddConfigChangedCallback(Config::ConfigChangedCallback func);

void RemoveConfigChangedCallback(ConfigChangedCallbackID callback_id);

// Called periodically by the CPU thread.
void CheckForConfigChanges();
}  // namespace CPUThreadConfigCallback
