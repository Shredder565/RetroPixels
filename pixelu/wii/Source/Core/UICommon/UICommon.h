// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

struct WindowSystemInfo;

// Start-up and shut-down shared by every front end: configuration, user folders, controllers,
// locale and a few host helpers.
namespace UICommon
{
void Init();
void Shutdown();

// Callbacks run by FlushUnsavedData, for hosts that may be killed without warning.
[[nodiscard]] Common::EventHook AddFlushUnsavedDataCallback(std::function<void()> callback);
void FlushUnsavedData();

void InitControllers(const WindowSystemInfo& wsi);
void ShutdownControllers();

void InhibitScreenSaver(bool inhibit);

// Makes `locale_name` the global C and C++ locale, falling back to shorter forms of the name
// and then to the system locale if it is not available.
void SetLocale(std::string locale_name);

void CreateDirectories();
// Chooses the user folder: `custom_path` if given, otherwise the platform's usual place.
void SetUserDirectory(std::string custom_path);

// Presses the console's power button if the running title listens for it.
bool TriggerSTMPowerEvent();

// A byte count in binary units with the given number of decimals, such as "1.08 MiB".
std::string FormatSize(u64 bytes, int decimals = 2);
}  // namespace UICommon
